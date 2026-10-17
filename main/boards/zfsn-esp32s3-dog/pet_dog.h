#ifndef PET_DOG_H
#define PET_DOG_H

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// Default stand/sleep angles; per-posture overrides may be stored in NVS.
#define STAND_ANGLE_LF             90
#define STAND_ANGLE_RF             90
#define STAND_ANGLE_LB             90
#define STAND_ANGLE_RB             90

#define SLEEP_ANGLE_LF             180
#define SLEEP_ANGLE_RF             180
#define SLEEP_ANGLE_LB             0
#define SLEEP_ANGLE_RB             0

#define SITDOWN_ANGLE_LF           90
#define SITDOWN_ANGLE_RF           90
#define SITDOWN_ANGLE_LB           30
#define SITDOWN_ANGLE_RB           30

#define SPEED                      40
#define SPEED_MODE                 200

// MCP schema defaults for the self.dog.action tool
#define DEFAULT_STEPS              3
#define DEFAULT_TURN_ANGLE         30

#define LEDC_TIMER              LEDC_TIMER_1
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY          (50)
#define LEDC_MIN_DUTY           (8191 * 0.025)      //205
#define LEDC_MAX_DUTY           (8191 * 0.13)       //1065
#define per_angle               (((LEDC_MAX_DUTY) - (LEDC_MIN_DUTY)) / 180)

#define CHANNEL_0               LEDC_CHANNEL_0
#define CHANNEL_1               LEDC_CHANNEL_1
#define CHANNEL_2               LEDC_CHANNEL_2
#define CHANNEL_3               LEDC_CHANNEL_3

typedef enum
{
    LEG1,
    LEG2,
    LEG3,
    LEG4
} leg_index;

typedef struct
{
    uint8_t angle;
    leg_index index;
    int speed;
} target_angle_config_t;

class PetDog
{
public:
    PetDog();
    ~PetDog();
    void InitializeDog(gpio_num_t LEDC_OUTPUT_IO_1, gpio_num_t LEDC_OUTPUT_IO_2, gpio_num_t LEDC_OUTPUT_IO_3, gpio_num_t LEDC_OUTPUT_IO_4);
    void ActionTask();
    void ActionIdleTask();
    void OnActionTask(std::function<void()> callback);

    void setFrontStep(int step_num) { front_step = step_num; }
    void setBackStep(int step_num) { back_step = step_num; }
    void setLeftAngle(int angle_num) { left_angle = angle_num; }
    void setRightAngle(int angle_num) { right_angle = angle_num; }

    int getFrontStep() { return front_step; }
    int getBackStep() { return back_step; }
    int getLeftAngle() { return left_angle; }
    int getRightAngle() { return right_angle; }

    void petsleep();
    void stand();
    void sitdown();
    void stretch();
    void stretch2();
    void scratching();
    void swing();

    void walkBack();
    void walkfront();
    void turnLeft();
    void turnRight();
    void petwave();
    void jump();

    void stop();
    void idle_activate(int rand_actin);

    // Custom action and angle persistence API
    bool SaveAngleSettings(const std::string& content, const std::string& posture = "stand");
    std::string GetAngleSettings() const;
    bool SaveCustomAction(const std::string& name, const std::string& content);
    std::string LoadCustomAction(const std::string& name);
    bool DeleteCustomAction(const std::string& name);
    std::vector<std::string> ListCustomActions();
    bool ExecuteCustomAction(const std::string& name);
    void ExecuteRandomExploreAction();                    // free_explore idle behaviour

    void Action(int action);
    // Accepted commands run on one worker. A new command supersedes the old one.
    bool DoAction(const char* name, int steps = DEFAULT_STEPS, int angle = DEFAULT_TURN_ANGLE);
    // Conversation state changes are lower priority than explicit commands.
    bool TryAutomaticPosture(const char* name);
    bool IsActionRunning() const;

    void SetFreeExplore(bool enabled);
    bool GetFreeExplore() const { return free_explore_.load(); }

private:
    struct CustomStep {
        std::array<uint8_t, 4> angles;
        uint32_t delay_ms;
    };
    struct ActionRequest {
        std::string name;
        int steps = DEFAULT_STEPS;
        int angle = DEFAULT_TURN_ANGLE;
        uint32_t generation = 0;
        std::vector<CustomStep> custom_steps;
    };
    using Postures = std::array<std::array<uint8_t, 4>, 3>;
    mutable std::mutex angles_mutex_;
    mutable std::mutex command_mutex_;
    std::mutex storage_mutex_;
    std::mutex servo_mutex_;
    Postures postures_ = {{{90, 90, 90, 90}, {180, 180, 0, 0}, {90, 90, 30, 30}}};
    ActionRequest pending_action_;
    bool has_pending_action_ = false;
    bool explicit_posture_hold_ = false; // Protected by command_mutex_.
    TaskHandle_t worker_task_ = nullptr;
    TaskHandle_t servo_task_ = nullptr;
    std::atomic<uint32_t> generation_{0};
    uint32_t active_generation_ = 0;
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> worker_exited_{true};
    std::atomic<bool> servo_exited_{true};
    std::atomic<bool> action_running_{false};
    std::atomic<bool> servo_running_{false};
    std::array<bool, 4> moving_{};
    std::array<TickType_t, 4> next_servo_tick_{};
    uint32_t servo_generation_ = 0;
    TickType_t next_explore_tick_ = 0;

    target_angle_config_t lf_;
    target_angle_config_t rf_;
    target_angle_config_t lb_;
    target_angle_config_t rb_;

    target_angle_config_t *legs[4] = { &lf_, &rf_, &lb_, &rb_ };

    int left_front_angle = SLEEP_ANGLE_LF - 5;
    int right_front_angle = SLEEP_ANGLE_RF;
    int left_back_angle = SLEEP_ANGLE_LB;
    int right_back_angle = SLEEP_ANGLE_RB + 5;

    int front_step = DEFAULT_STEPS;
    int back_step = DEFAULT_STEPS;

    int left_angle = DEFAULT_TURN_ANGLE;
    int right_angle = DEFAULT_TURN_ANGLE;

    std::atomic<bool> free_explore_{false};

    ledc_timer_config_t ledc_timer_;
    std::function<void()> action_task_;

    void to_any_angle_task(uint8_t lf_angle, uint8_t rf_angle, uint8_t lb_angle, uint8_t rb_angle);
    void AngleTask();
    void WriteServo(size_t index, int angle);
    void WriteServoLocked(size_t index, int angle);
    void set_angle(uint8_t lf_angle, uint8_t rf_angle, uint8_t lb_angle, uint8_t rb_angle);

    void set_left_front_angle(int angle);
    void set_right_front_angle(int angle);
    void set_left_back_angle(int angle);
    void set_right_back_angle(int angle);

    ledc_channel_config_t create_ledc_channel(gpio_num_t gpio, int channel);
    void set_speed(int speed);
    bool IsInterrupted() const;
    bool WaitTicks(TickType_t ticks);
    bool Delay(uint32_t milliseconds);
    bool EnqueueAction(ActionRequest request, bool automatic = false);
    bool IsActionRunningLocked() const;
    void ExecuteAction(const ActionRequest& request);
    void ApplyPosture(size_t index);
    void LoadAngleSettings();
    static bool ParseAngleSettings(const std::string& content, const std::string& posture, Postures& result);
    static std::string SerializeAngleSettings(const Postures& postures);
    static bool ParseCustomAction(const std::string& content, std::vector<CustomStep>& steps);
    static bool CustomActionPath(const std::string& name, std::string& path);
};

#endif // PET_DOG_H
