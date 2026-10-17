#include "pet_dog.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include <esp_log.h>
#include <cJSON.h>
#include <nvs.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <utility>

#define TAG "PetDog"

namespace {
constexpr size_t kMaxActionBytes = 8192;
constexpr const char* kCustomActionDirectory = "/sdcard/custom_action";
// This unit's right-rear leg needs a small rearward alignment correction.
// RB is mirrored (180 - logical angle), so a -2 degree PWM trim corresponds
// to logical +2. Keep trajectory/NVS angles untrimmed to avoid feedback drift.
constexpr int kRightRearServoTrimDegrees = -2;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

Json ParseJson(const std::string& content) {
    if (content.empty() || content.size() > kMaxActionBytes || content.find('\0') != std::string::npos)
        return Json(nullptr, cJSON_Delete);
    return Json(cJSON_ParseWithLengthOpts(content.c_str(), content.size() + 1, nullptr, true), cJSON_Delete);
}

bool IntegerInRange(const cJSON* value, int minimum, int maximum) {
    return cJSON_IsNumber(value) && value->valuedouble >= minimum &&
        value->valuedouble <= maximum && value->valuedouble == value->valueint;
}

bool ParseFourAngles(const cJSON* object, std::array<uint8_t, 4>& angles) {
    if (!cJSON_IsObject(object)) return false;
    const char* names[] = {"lf", "rf", "lb", "rb"};
    for (size_t i = 0; i < angles.size(); ++i) {
        auto value = cJSON_GetObjectItemCaseSensitive(object, names[i]);
        if (!IntegerInRange(value, 0, 180)) return false;
        angles[i] = value->valueint;
    }
    return true;
}
}

PetDog::PetDog() = default;

PetDog::~PetDog() {
    shutting_down_.store(true);
    generation_.fetch_add(1);
    if (worker_task_) xTaskNotifyGive(worker_task_);
    if (servo_task_) xTaskNotifyGive(servo_task_);
    while (!worker_exited_.load() || !servo_exited_.load()) vTaskDelay(1);
}

void PetDog::InitializeDog(gpio_num_t io1, gpio_num_t io2, gpio_num_t io3, gpio_num_t io4) {
    if (worker_task_ || servo_task_) return;
    LoadAngleSettings();
    ledc_timer_ = {};
    ledc_timer_.speed_mode = LEDC_MODE;
    ledc_timer_.duty_resolution = LEDC_DUTY_RES;
    ledc_timer_.timer_num = LEDC_TIMER;
    ledc_timer_.freq_hz = LEDC_FREQUENCY;
    ledc_timer_.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer_));
    // Servo order: IO1=14/RB, IO2=17/LF, IO3=13/RF, IO4=18/LB.
    gpio_num_t pins[] = {io1, io2, io3, io4};
    for (int channel = 0; channel < 4; ++channel) {
        auto config = create_ledc_channel(pins[channel], channel);
        ESP_ERROR_CHECK(ledc_channel_config(&config));
        ledc_stop(LEDC_MODE, static_cast<ledc_channel_t>(channel), 0);
    }
    ESP_LOGI(TAG, "Servo output trim: RB GPIO%d %+d deg; LF/RF/LB unchanged",
             int(io1), kRightRearServoTrimDegrees);
    lf_.index = LEG1;
    rf_.index = LEG2;
    lb_.index = LEG3;
    rb_.index = LEG4;
    set_speed(10);
    // The sleep pose starts with LF -5 and RB +5.
    auto sleep = postures_[1];
    set_angle(std::max(0, int(sleep[0]) - 5), sleep[1], sleep[2], std::min(180, int(sleep[3]) + 5));

    servo_exited_.store(false);
    if (xTaskCreate([](void* arg) { static_cast<PetDog*>(arg)->AngleTask(); },
                    "AngTask", 4096, this, 1, &servo_task_) != pdPASS) {
        servo_exited_.store(true);
        ESP_LOGE(TAG, "Failed to create servo worker");
        return;
    }
    worker_exited_.store(false);
    if (xTaskCreate([](void* arg) { static_cast<PetDog*>(arg)->ActionTask(); },
                    "ActWkr", 4096, this, 1, &worker_task_) != pdPASS) {
        worker_exited_.store(true);
        ESP_LOGE(TAG, "Failed to create action worker");
    }
}

bool PetDog::IsInterrupted() const {
    return shutting_down_.load() || generation_.load() != active_generation_;
}

bool PetDog::WaitTicks(TickType_t ticks) {
    const auto slice = std::max<TickType_t>(1, pdMS_TO_TICKS(20));
    while (ticks && !IsInterrupted()) {
        auto wait = std::min(ticks, slice);
        vTaskDelay(wait);
        ticks -= wait;
    }
    return !IsInterrupted();
}

bool PetDog::Delay(uint32_t milliseconds) {
    return WaitTicks(milliseconds ? std::max<TickType_t>(1, (uint64_t(milliseconds) * configTICK_RATE_HZ) / 1000) : 0);
}

void PetDog::OnActionTask(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    action_task_ = std::move(callback);
    if (worker_task_) xTaskNotifyGive(worker_task_);
}

void PetDog::ActionTask() {
    while (!shutting_down_.load()) {
        ActionRequest request;
        std::function<void()> callback;
        bool have_request = false;
        {
            std::lock_guard<std::mutex> lock(command_mutex_);
            if (has_pending_action_) {
                request = std::move(pending_action_);
                has_pending_action_ = false;
                have_request = true;
                action_running_.store(true);
            } else {
                callback = action_task_;
            }
        }
        if (have_request) {
            active_generation_ = request.generation;
            set_speed(10);
            ESP_LOGI(TAG, "Action started #%lu %s", static_cast<unsigned long>(request.generation), request.name.c_str());
            if (!IsInterrupted()) ExecuteAction(request);
            // Posture dispatch schedules interpolation on AngleTask. Keep the
            // command active until those PWM targets finish or it is replaced.
            while (!IsInterrupted() && servo_running_.load()) WaitTicks(pdMS_TO_TICKS(10));
            ESP_LOGI(TAG, "Action %s #%lu %s", IsInterrupted() ? "superseded" : "finished",
                     static_cast<unsigned long>(request.generation), request.name.c_str());
            action_running_.store(false);
        } else {
            if (callback) {
                active_generation_ = generation_.load();
                callback();
            }
            ActionIdleTask();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(callback ? 100 : 1000));
        }
    }
    worker_exited_.store(true);
    vTaskDelete(nullptr);
}

void PetDog::ActionIdleTask() {
    if (!free_explore_.load() || Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        next_explore_tick_ = 0;
        return;
    }
    auto now = xTaskGetTickCount();
    if (!next_explore_tick_) {
        // First idle action runs at a random 60..180 minute interval.
        next_explore_tick_ = now + pdMS_TO_TICKS(((rand() % 121) + 60) * 60 * 1000);
    } else if (static_cast<int32_t>(now - next_explore_tick_) >= 0) {
        next_explore_tick_ = 0;
        ExecuteRandomExploreAction();
    }
}

void PetDog::SetFreeExplore(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        // Explicitly enabling autonomous exploration releases a held posture.
        if (enabled) explicit_posture_hold_ = false;
    }
    free_explore_.store(enabled);
    if (worker_task_) xTaskNotifyGive(worker_task_);
}

void PetDog::ExecuteRandomExploreAction() {
    ActionRequest request;
    request.name = "__explore";
    request.angle = rand() % 3;
    EnqueueAction(std::move(request), true);
}

void PetDog::idle_activate(int rand_action)
{
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        display->SetEmotion("sleepy");   // TODO: custom start_emtion/idle_emtion UI
    }
    if(rand_action == 0)
    {
        stretch();
    }else if (rand_action == 1)
    {
        stretch2();
    }else if (rand_action == 2)
    {
        scratching();
    }
    if (!Delay(3000)) return;
    petsleep();
    if (display != nullptr) {
        display->SetEmotion("neutral");
    }
}

void PetDog::stand()
{
    ApplyPosture(0);
}

void PetDog::sitdown()
{
    ApplyPosture(2);
}

void PetDog::petsleep()
{
    ApplyPosture(1);
    if (!Delay(5000)) return;
    std::lock_guard<std::mutex> lock(servo_mutex_);
    if (IsInterrupted()) return;
    moving_.fill(false);
    servo_running_.store(false);
    ledc_stop(LEDC_MODE, CHANNEL_0, 0);
    ledc_stop(LEDC_MODE, CHANNEL_1, 0);
    ledc_stop(LEDC_MODE, CHANNEL_2, 0);
    ledc_stop(LEDC_MODE, CHANNEL_3, 0);
}

void PetDog::stretch()
{
    set_speed(20);
    ApplyPosture(0);
    if (!Delay(2000)) return;
    to_any_angle_task(10,10,45,45);
    if (!Delay(2000)) return;
    to_any_angle_task(135,135,170,170);
    if (!Delay(2000)) return;
    ApplyPosture(0);

    set_speed(10);

}

void PetDog::stretch2()
{
    set_speed(20);
    ApplyPosture(0);
    if (!Delay(2000)) return;
    to_any_angle_task(0,0,180,180);
    if (!Delay(2000)) return;
    ApplyPosture(0);

    set_speed(10);

}

void PetDog::scratching()
{
    set_speed(20);

    ApplyPosture(0);
    if (!Delay(2000)) return;
    to_any_angle_task(90,180,0,0);
    if (!Delay(1200)) return;
    int wave_num = 5;
    while(wave_num -- != 0)
    {
        if (IsInterrupted()) return;
        for (size_t i = 0; i < 45; i+=4)
        {
            set_left_back_angle(i);
            if (!Delay(15)) return;
        }
        for (size_t i = 44; i > 0; i-=4)
        {
            set_left_back_angle(i);
            if (!Delay(25)) return;
        }
    }
    if (!Delay(1000)) return;
    stand();

    set_speed(10);
}

void PetDog::walkfront()
{
    int step = getFrontStep();
    while(step--)
    {
        if (IsInterrupted()) return;
        set_angle(90,45,45,90);
        set_angle(135,45,45,135);
        set_angle(135,90,90,135);
        set_angle(90,90,90,90);
        set_angle(45,90,90,45);
        set_angle(45,135,135,45);
        set_angle(90,135,135,90);
        set_angle(90,90,90,90);
    }
    stand();
}

void PetDog::walkBack()
{
    int step = getBackStep();
    while(step--)
    {
        if (IsInterrupted()) return;
        set_angle(90,90,90,90);
        set_angle(90,135,135,90);
        set_angle(45,135,135,45);
        set_angle(45,90,90,45);
        set_angle(90,90,90,90);
        set_angle(135,90,90,135);
        set_angle(135,45,45,135);
        set_angle(90,45,45,90);
    }
    stand();
}

void PetDog::turnLeft()
{
    int left_angle = getLeftAngle();
    int step = left_angle / 12;
    while(step--)
    {
        if (IsInterrupted()) return;
        set_right_front_angle(90);
        if (!Delay(SPEED)) return;
        set_right_back_angle(90);
        if (!Delay(SPEED)) return;
        set_left_front_angle(90);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(90);
        if (!Delay(SPEED)) return;

        set_right_front_angle(90);
        if (!Delay(SPEED)) return;
        set_right_back_angle(50);
        if (!Delay(SPEED)) return;
        set_left_front_angle(130);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(90);
        if (!Delay(SPEED)) return;

        set_right_front_angle(130);
        if (!Delay(SPEED)) return;
        set_right_back_angle(50);
        if (!Delay(SPEED)) return;
        set_left_front_angle(130);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(50);
        if (!Delay(SPEED)) return;

        set_right_front_angle(130);
        if (!Delay(SPEED)) return;
        set_right_back_angle(90);
        if (!Delay(SPEED)) return;
        set_left_front_angle(90);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(50);
        if (!Delay(SPEED)) return;
    }
    stand();
}

void PetDog::turnRight()
{
    int right_angle = getRightAngle();
    int step = right_angle / 12;
    while(step--)
    {
        if (IsInterrupted()) return;
        set_right_front_angle(130);
        if (!Delay(SPEED)) return;
        set_right_back_angle(90);
        if (!Delay(SPEED)) return;
        set_left_front_angle(90);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(50);
        if (!Delay(SPEED)) return;

        set_right_front_angle(130);
        if (!Delay(SPEED)) return;
        set_right_back_angle(50);
        if (!Delay(SPEED)) return;
        set_left_front_angle(130);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(50);
        if (!Delay(SPEED)) return;

        set_right_front_angle(90);
        if (!Delay(SPEED)) return;
        set_right_back_angle(50);
        if (!Delay(SPEED)) return;
        set_left_front_angle(130);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(90);
        if (!Delay(SPEED)) return;

        set_right_front_angle(90);
        if (!Delay(SPEED)) return;
        set_right_back_angle(90);
        if (!Delay(SPEED)) return;
        set_left_front_angle(90);   //1
        if (!Delay(SPEED)) return;
        set_left_back_angle(90);
        if (!Delay(SPEED)) return;
    }
    stand();
}

void PetDog::jump()
{
    // Jump sequence: fixed angles and RTOS tick waits.
    ApplyPosture(0);
    if (!WaitTicks(10)) return;
    to_any_angle_task(50, 50, 30, 30);
    if (!WaitTicks(100)) return;
    set_left_front_angle(135);
    set_right_front_angle(135);
    set_left_back_angle(135);
    set_right_back_angle(135);
    if (!WaitTicks(50)) return;
    stand();
}

void PetDog::stop()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    auto generation = generation_.fetch_add(1) + 1;
    explicit_posture_hold_ = true;
    if (has_pending_action_) {
        ESP_LOGI(TAG, "Action superseded-before-start #%lu %s",
                 static_cast<unsigned long>(pending_action_.generation), pending_action_.name.c_str());
    }
    has_pending_action_ = false;
    {
        std::lock_guard<std::mutex> servo_lock(servo_mutex_);
        moving_.fill(false);
        servo_running_.store(false);
    }
    ESP_LOGI(TAG, "Action finished #%lu stop", static_cast<unsigned long>(generation));
    if (worker_task_) xTaskNotifyGive(worker_task_);
}

void PetDog::petwave()
{
    to_any_angle_task(90,90,50,0);
    if (!Delay(600)) return;
    int wave_num = 5;
    while(wave_num -- != 0)
    {
        for (size_t i = 0; i < 65; i+=4)
        {
            set_left_front_angle(i);
            if (!Delay(25)) return;
        }
        for (size_t i = 64; i > 0; i-=4)
        {
            set_left_front_angle(i);
            if (!Delay(25)) return;
        }
    }
    if (!Delay(1000)) return;
    stand();
}

void PetDog::swing()
{
    to_any_angle_task(90,90,90,90);
    if (!Delay(600)) return;
    int wave_num = 5;

    srand((unsigned)time(NULL));
    int random_number = rand() % 100;
    if(random_number < 50)
    {
        while(wave_num -- != 0)
        {
            for (size_t i = 0; i < 45; i+=5)
            {
                set_left_front_angle(90+i);
                set_right_front_angle(90+i);
                set_left_back_angle(90+i);
                set_right_back_angle(90+i);
                if (!Delay(25)) return;
            }
            for (size_t i = 0; i < 90; i+=5)
            {
                set_left_front_angle(135-i);
                set_right_front_angle(135-i);
                set_left_back_angle(135-i);
                set_right_back_angle(135-i);
                if (!Delay(25)) return;
            }

            for (size_t i = 0; i < 45; i+=5)
            {
                set_left_front_angle(45+i);
                set_right_front_angle(45+i);
                set_left_back_angle(45+i);
                set_right_back_angle(45+i);
                if (!Delay(25)) return;
            }
        }
    }else
    {
        while(wave_num -- != 0)
        {
            for (size_t i = 0; i < 45; i+=5)
            {
                set_left_front_angle(90+i);
                set_right_front_angle(90+i);
                set_left_back_angle(90-i);
                set_right_back_angle(90-i);
                if (!Delay(25)) return;
            }
            for (size_t i = 0; i < 90; i+=5)
            {
                set_left_front_angle(135-i);
                set_right_front_angle(135-i);
                set_left_back_angle(45+i);
                set_right_back_angle(45+i);
                if (!Delay(25)) return;
            }

            for (size_t i = 0; i < 45; i+=5)
            {
                set_left_front_angle(45+i);
                set_right_front_angle(45+i);
                set_left_back_angle(135-i);
                set_right_back_angle(135-i);
                if (!Delay(25)) return;
            }
        }
    }

    if (!Delay(1000)) return;
    stand();
}


bool PetDog::IsActionRunningLocked() const {
    return has_pending_action_ || action_running_.load() || servo_running_.load();
}

bool PetDog::IsActionRunning() const {
    std::lock_guard<std::mutex> lock(command_mutex_);
    return IsActionRunningLocked();
}

bool PetDog::EnqueueAction(ActionRequest request, bool automatic) {
    if (!worker_task_ || !servo_task_ || shutting_down_.load()) return false;
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (automatic && (explicit_posture_hold_ || IsActionRunningLocked())) return false;
    if (!automatic) {
        explicit_posture_hold_ = request.name == "stand" || request.name == "wake" ||
                                 request.name == "sitdown" || request.name == "sleep";
    }
    if (has_pending_action_) {
        ESP_LOGI(TAG, "Action superseded-before-start #%lu %s",
                 static_cast<unsigned long>(pending_action_.generation), pending_action_.name.c_str());
    }
    request.generation = generation_.fetch_add(1) + 1;
    pending_action_ = std::move(request);
    has_pending_action_ = true;
    ESP_LOGI(TAG, "Action accepted #%lu %s (%s)", static_cast<unsigned long>(pending_action_.generation),
             pending_action_.name.c_str(), automatic ? "automatic" : "explicit");
    xTaskNotifyGive(worker_task_);
    return true;
}

bool PetDog::TryAutomaticPosture(const char* name) {
    if (!name || (strcmp(name, "stand") != 0 && strcmp(name, "sleep") != 0 && strcmp(name, "sitdown") != 0)) return false;
    ActionRequest request;
    request.name = name;
    return EnqueueAction(std::move(request), true);
}

bool PetDog::DoAction(const char* name, int steps, int angle) {
    if (!name || !*name) return false;
    // Keep one canonical execution path while accepting common tool-call aliases.
    struct Alias { const char* name; const char* canonical; };
    static constexpr Alias aliases[] = {
        {"sit", "sitdown"}, {"crouch", "sitdown"}, {"squat", "sitdown"},
        {"坐下", "sitdown"}, {"蹲下", "sitdown"}, {"lie_down", "sleep"},
        {"趴下", "sleep"}, {"forward", "walk_forward"}, {"backward", "walk_back"},
        {"walk_backward", "walk_back"}, {"dance", "swing"},
    };
    for (const auto& alias : aliases) {
        if (strcmp(name, alias.name) == 0) { name = alias.canonical; break; }
    }
    const char* names[] = {"walk_forward", "walk_back", "turn_left", "turn_right", "sitdown",
        "stand", "sleep", "stretch", "scratching", "swing", "petwave", "stop", "jump", "wake"};
    if (std::none_of(std::begin(names), std::end(names), [name](const char* item) {
            return strcmp(name, item) == 0;
        })) return false;
    if (strcmp(name, "stop") == 0) { stop(); return true; }
    if ((strcmp(name, "walk_forward") == 0 || strcmp(name, "walk_back") == 0) &&
        (steps < 1 || steps > 100)) return false;
    if ((strcmp(name, "turn_left") == 0 || strcmp(name, "turn_right") == 0) &&
        (angle < 1 || angle > 360)) return false;
    ActionRequest request;
    request.name = name;
    request.steps = steps;
    request.angle = angle;
    return EnqueueAction(std::move(request));
}

void PetDog::ExecuteAction(const ActionRequest& request) {
    setFrontStep(request.steps);
    setBackStep(request.steps);
    setLeftAngle(request.angle);
    setRightAngle(request.angle);
    const auto& action = request.name;
    if (action == "__custom") {
        for (const auto& step : request.custom_steps) {
            if (IsInterrupted()) return;
            to_any_angle_task(step.angles[0], step.angles[1], step.angles[2], step.angles[3]);
            if (!Delay(step.delay_ms)) return;
        }
    } else if (action == "__explore") idle_activate(request.angle);
    else if (action == "walk_forward") walkfront();
    else if (action == "walk_back") walkBack();
    else if (action == "turn_left") turnLeft();
    else if (action == "turn_right") turnRight();
    else if (action == "sitdown") sitdown();
    else if (action == "stand" || action == "wake") stand();
    else if (action == "sleep") petsleep();
    else if (action == "stretch") { if (rand() % 2) stretch2(); else stretch(); }
    else if (action == "scratching") scratching();
    else if (action == "swing") swing();
    else if (action == "petwave") petwave();
    else if (action == "jump") jump();
}

void PetDog::Action(int action) {
    ESP_LOGW(TAG, "Unsupported legacy action enum: %d", action);
}

void PetDog::ApplyPosture(size_t index) {
    std::array<uint8_t, 4> posture;
    {
        std::lock_guard<std::mutex> lock(angles_mutex_);
        posture = postures_[index];
    }
    to_any_angle_task(posture[0], posture[1], posture[2], posture[3]);
}

bool PetDog::ParseAngleSettings(const std::string& content, const std::string& posture, Postures& result) {
    auto root = ParseJson(content);
    if (!cJSON_IsObject(root.get())) return false;
    const char* names[] = {"stand", "sleep", "sit"};
    bool found = false;
    for (size_t i = 0; i < result.size(); ++i) {
        auto item = cJSON_GetObjectItemCaseSensitive(root.get(), names[i]);
        if (!item) continue;
        if (!ParseFourAngles(item, result[i])) return false;
        found = true;
    }
    if (!found) {
        size_t index;
        if (posture == "stand") index = 0;
        else if (posture == "sleep") index = 1;
        else if (posture == "sit" || posture == "sitdown") index = 2;
        else return false;
        return ParseFourAngles(root.get(), result[index]);
    }
    return true;
}

std::string PetDog::SerializeAngleSettings(const Postures& postures) {
    char json[256];
    snprintf(json, sizeof(json),
        "{\"stand\":{\"lf\":%u,\"rf\":%u,\"lb\":%u,\"rb\":%u},"
        "\"sleep\":{\"lf\":%u,\"rf\":%u,\"lb\":%u,\"rb\":%u},"
        "\"sit\":{\"lf\":%u,\"rf\":%u,\"lb\":%u,\"rb\":%u}}",
        postures[0][0], postures[0][1], postures[0][2], postures[0][3],
        postures[1][0], postures[1][1], postures[1][2], postures[1][3],
        postures[2][0], postures[2][1], postures[2][2], postures[2][3]);
    return json;
}

bool PetDog::SaveAngleSettings(const std::string& content, const std::string& posture) {
    std::lock_guard<std::mutex> lock(angles_mutex_);
    auto updated = postures_;
    if (!ParseAngleSettings(content, posture, updated)) {
        ESP_LOGE(TAG, "Failed to parse angle settings");
        return false;
    }
    auto json = SerializeAngleSettings(updated);
    nvs_handle_t handle;
    auto error = nvs_open("petdog", NVS_READWRITE, &handle);
    if (error != ESP_OK) return false;
    error = nvs_set_str(handle, "angle_settings", json.c_str());
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save angle settings: %s", esp_err_to_name(error));
        return false;
    }
    postures_ = updated;
    ESP_LOGI(TAG, "Angle settings persisted to NVS");
    return true;
}

std::string PetDog::GetAngleSettings() const {
    std::lock_guard<std::mutex> lock(angles_mutex_);
    return SerializeAngleSettings(postures_);
}

void PetDog::LoadAngleSettings() {
    nvs_handle_t handle;
    if (nvs_open("petdog", NVS_READONLY, &handle) != ESP_OK) return;
    size_t length = 0;
    auto error = nvs_get_str(handle, "angle_settings", nullptr, &length);
    std::string json;
    if (error == ESP_OK && length > 1 && length <= kMaxActionBytes + 1) {
        json.resize(length);
        error = nvs_get_str(handle, "angle_settings", json.data(), &length);
        if (error == ESP_OK) json.resize(length - 1);
    }
    nvs_close(handle);
    std::lock_guard<std::mutex> lock(angles_mutex_);
    auto updated = postures_;
    if (error == ESP_OK && ParseAngleSettings(json, "stand", updated)) {
        postures_ = updated;
        ESP_LOGI(TAG, "Loading saved angle settings: %s", json.c_str());
    } else {
        ESP_LOGI(TAG, "No saved angle settings found, using defaults");
    }
}

bool PetDog::ParseCustomAction(const std::string& content, std::vector<CustomStep>& steps) {
    auto root = ParseJson(content);
    auto array = cJSON_GetObjectItemCaseSensitive(root.get(), "steps");
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) == 0) return false;
    std::vector<CustomStep> parsed;
    const cJSON* item;
    cJSON_ArrayForEach(item, array) {
        CustomStep step;
        auto delay = cJSON_GetObjectItemCaseSensitive(item, "delay");
        if (!ParseFourAngles(item, step.angles) || !IntegerInRange(delay, 0, INT_MAX)) return false;
        step.delay_ms = delay->valueint;
        parsed.push_back(step);
    }
    steps = std::move(parsed);
    return true;
}

bool PetDog::CustomActionPath(const std::string& name, std::string& path) {
    if (name.empty() || name.size() > 250 || name == "." || name == "..") return false;
    for (unsigned char ch : name) {
        if (ch < 32 || ch == 127 || strchr("/\\:*?\"<>|", ch)) return false;
    }
    if (name.back() == '.' || name.back() == ' ') return false;
    path = std::string(kCustomActionDirectory) + "/" + name;
    if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) path += ".json";
    return true;
}

bool PetDog::SaveCustomAction(const std::string& name, const std::string& content) {
    std::string path;
    std::vector<CustomStep> steps;
    if (!CustomActionPath(name, path) || !ParseCustomAction(content, steps)) return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    struct stat info;
    if (stat(kCustomActionDirectory, &info) != 0) {
        if (errno != ENOENT || mkdir(kCustomActionDirectory, 0775) != 0) return false;
    } else if (!S_ISDIR(info.st_mode)) return false;
    // Keep the previous action if receiving/writing its replacement fails.
    auto temporary = path + ".tmp";
    auto file = fopen(temporary.c_str(), "wb");
    if (!file) return false;
    bool success = fwrite(content.data(), 1, content.size(), file) == content.size();
    success = fclose(file) == 0 && success;
    // FatFS rename cannot replace an existing destination. Retain a rollback copy.
    auto backup = path + ".bak";
    bool had_previous = stat(path.c_str(), &info) == 0;
    if (had_previous && !S_ISREG(info.st_mode)) {
        remove(temporary.c_str());
        return false;
    }
    if (success && had_previous) {
        remove(backup.c_str());
        success = rename(path.c_str(), backup.c_str()) == 0;
    }
    if (success && rename(temporary.c_str(), path.c_str()) != 0) {
        if (had_previous) rename(backup.c_str(), path.c_str());
        success = false;
    }
    if (success && had_previous) remove(backup.c_str());
    if (!success) { remove(temporary.c_str()); return false; }
    ESP_LOGI(TAG, "Saved custom action: %s", name.c_str());
    return true;
}

std::string PetDog::LoadCustomAction(const std::string& name) {
    std::string path;
    if (!CustomActionPath(name, path)) return {};
    std::lock_guard<std::mutex> lock(storage_mutex_);
    struct stat info;
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > kMaxActionBytes) return {};
    auto file = fopen(path.c_str(), "rb");
    if (!file) return {};
    std::string content(info.st_size, '\0');
    bool success = fread(content.data(), 1, content.size(), file) == content.size();
    success = fclose(file) == 0 && success;
    return success ? content : std::string();
}

bool PetDog::DeleteCustomAction(const std::string& name) {
    std::string path;
    if (!CustomActionPath(name, path)) return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    return remove(path.c_str()) == 0;
}

std::vector<std::string> PetDog::ListCustomActions() {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::vector<std::string> names;
    auto directory = opendir(kCustomActionDirectory);
    if (!directory) return names;
    while (auto item = readdir(directory)) {
        std::string name(item->d_name);
        if (name.size() <= 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        std::string path;
        struct stat info;
        if (CustomActionPath(name, path) && stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode))
            names.push_back(name.substr(0, name.size() - 5));
    }
    closedir(directory);
    std::sort(names.begin(), names.end());
    return names;
}

bool PetDog::ExecuteCustomAction(const std::string& name) {
    auto content = LoadCustomAction(name);
    ActionRequest request;
    request.name = "__custom";
    if (!ParseCustomAction(content, request.custom_steps)) return false;
    return EnqueueAction(std::move(request));
}

void PetDog::to_any_angle_task(uint8_t lf, uint8_t rf, uint8_t lb, uint8_t rb) {
    if (IsInterrupted()) return;
    std::lock_guard<std::mutex> lock(servo_mutex_);
    if (IsInterrupted()) return;
    uint8_t targets[] = {lf, rf, lb, rb};
    servo_generation_ = active_generation_;
    for (size_t i = 0; i < 4; ++i) {
        legs[i]->angle = std::min<uint8_t>(targets[i], 180);
        moving_[i] = true;
        next_servo_tick_[i] = xTaskGetTickCount();
    }
    servo_running_.store(true);
    if (servo_task_) xTaskNotifyGive(servo_task_);
}

void PetDog::AngleTask() {
    while (!shutting_down_.load()) {
        {
            std::lock_guard<std::mutex> lock(servo_mutex_);
            if (servo_generation_ != generation_.load()) moving_.fill(false);
            auto now = xTaskGetTickCount();
            int* current[] = {&left_front_angle, &right_front_angle, &left_back_angle, &right_back_angle};
            for (size_t i = 0; i < 4; ++i) {
                if (!moving_[i] || static_cast<int32_t>(now - next_servo_tick_[i]) < 0) continue;
                auto target = legs[i]->angle;
                int next = *current[i] + (*current[i] < target ? 1 : (*current[i] > target ? -1 : 0));
                WriteServoLocked(i, next);
                moving_[i] = next != target;
                next_servo_tick_[i] = now + std::max<TickType_t>(1, pdMS_TO_TICKS(legs[i]->speed));
            }
            servo_running_.store(std::any_of(moving_.begin(), moving_.end(), [](bool moving) { return moving; }));
        }
        ulTaskNotifyTake(pdTRUE, std::max<TickType_t>(1, pdMS_TO_TICKS(10)));
    }
    servo_exited_.store(true);
    vTaskDelete(nullptr);
}

void PetDog::WriteServoLocked(size_t index, int angle) {
    angle = std::clamp(angle, 0, 180);
    int* current[] = {&left_front_angle, &right_front_angle, &left_back_angle, &right_back_angle};
    *current[index] = angle;
    const ledc_channel_t channels[] = {CHANNEL_1, CHANNEL_2, CHANNEL_3, CHANNEL_0};
    int physical_angle = (index == 1 || index == 3) ? 180 - angle : angle;
    if (index == 3) physical_angle = std::clamp(physical_angle + kRightRearServoTrimDegrees, 0, 180);
    ledc_set_duty(LEDC_MODE, channels[index], physical_angle * per_angle + LEDC_MIN_DUTY);
    ledc_update_duty(LEDC_MODE, channels[index]);
}

void PetDog::WriteServo(size_t index, int angle) {
    if (IsInterrupted()) return;
    std::lock_guard<std::mutex> lock(servo_mutex_);
    if (IsInterrupted()) return;
    moving_[index] = false;
    WriteServoLocked(index, angle);
}

void PetDog::set_right_back_angle(int angle)
{
    WriteServo(3, angle);
}
void PetDog::set_left_front_angle(int angle)
{
    WriteServo(0, angle);
}
void PetDog::set_right_front_angle(int angle)
{
    WriteServo(1, angle);
}
void PetDog::set_left_back_angle(int angle)
{
    WriteServo(2, angle);
}

void PetDog::set_angle(uint8_t lf_angle,uint8_t rf_angle,uint8_t lb_angle,uint8_t rb_angle)
{
    set_left_front_angle(lf_angle);
    if (!Delay(40)) return;
    set_right_front_angle(rf_angle);
    if (!Delay(40)) return;
    set_left_back_angle(lb_angle);
    if (!Delay(40)) return;
    set_right_back_angle(rb_angle);
    if (!Delay(40)) return;
}

ledc_channel_config_t PetDog::create_ledc_channel(gpio_num_t gpio, int channel)
{
    ledc_channel_config_t ledc_channel = {
        .gpio_num       = gpio,
        .speed_mode     = LEDC_MODE,
        .channel        = (ledc_channel_t)channel,
        .intr_type      = LEDC_INTR_DISABLE,
        .timer_sel      = LEDC_TIMER,
        .duty           = 0,
        .hpoint         = 0
    };
    return ledc_channel;
}

void PetDog::set_speed(int speed)
{
    std::lock_guard<std::mutex> lock(servo_mutex_);
    lf_.speed = speed;
    rf_.speed = speed;
    lb_.speed = speed;
    rb_.speed = speed;
}
