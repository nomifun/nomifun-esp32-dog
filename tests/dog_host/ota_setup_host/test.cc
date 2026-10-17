#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_ERR_TIMEOUT = 0x107;
constexpr int MAIN_EVENT_CHECK_NEW_VERSION_DONE = 1;
enum DeviceState { kDeviceStateStarting, kDeviceStateActivating, kDeviceStateIdle, kDeviceStateFatalError };
#define TAG "Application"
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(value) (value)
namespace Lang {
namespace Strings {
constexpr auto CHECKING_NEW_VERSION = "Checking version";
constexpr auto CHECK_NEW_VERSION_FAILED = "Retry in %d seconds: %s";
constexpr auto ERROR = "Error";
constexpr auto ACTIVATION = "Activation";
}
namespace Sounds { constexpr std::string_view OGG_EXCLAMATION = "sound"; }
}

namespace {
std::vector<esp_err_t> responses;
std::size_t calls;
int elapsed_ms;
int completed_events;
int marked_valid;
int tests;
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
void vTaskDelay(int milliseconds) { elapsed_ms += milliseconds; }
void xEventGroupSetBits(int, int bits) { completed_events |= bits; }
struct Display { void SetStatus(const char*) {} };
struct Board {
    Display display;
    static Board& GetInstance() { static Board instance; return instance; }
    Display* GetDisplay() { return &display; }
};
struct Ota {
    esp_err_t CheckVersion() {
        Require(calls < responses.size(), "Exceeded intended CheckVersion call count");
        return responses[calls++];
    }
    std::string GetCheckVersionUrl() { return "https://invalid.example/ota"; }
    bool HasNewVersion() { return false; }
    void MarkCurrentVersionValid() { ++marked_valid; }
    bool HasActivationCode() { return false; }
    bool HasActivationChallenge() { return false; }
    std::string GetActivationCode() { return {}; }
    std::string GetActivationMessage() { return {}; }
    esp_err_t Activate() { return ESP_OK; }
};
struct Application {
    DeviceState device_state_ = kDeviceStateStarting;
    int event_group_ = 0;
    bool protocol_initialized = false;
    std::vector<std::string> alert_statuses;
    std::vector<std::string> alert_messages;
    void SetDeviceState(DeviceState state) { device_state_ = state; }
    void Alert(const char* status, const char* message, const char*, std::string_view) {
        alert_statuses.emplace_back(status);
        alert_messages.emplace_back(message);
    }
    bool UpgradeFirmware(Ota&) { return false; }
    void ShowActivationCode(const std::string&, const std::string&) {}
    bool CheckNewVersion(Ota& ota);
    void Start();
};

#include "actual_check_new_version.inc"
#include "actual_startup_gate.inc"

namespace {
void Seed(std::vector<esp_err_t> sequence) {
    responses = std::move(sequence);
    calls = 0;
    elapsed_ms = completed_events = marked_valid = 0;
}
void TestFailureBudget() {
#if defined(CONFIG_BOARD_TYPE_ZFSN_ESP32S3_DOG)
    Seed({-21, -22, -23});
#else
    Seed(std::vector<esp_err_t>(10, -23));
#endif
    Application app;
    app.Start();
    Require(completed_events == 0 && marked_valid == 0, "Failed OTA must not become valid/completed");
#if defined(CONFIG_BOARD_TYPE_ZFSN_ESP32S3_DOG)
    Require(calls == 3, "Production dog must stop after three failed requests");
    Require(elapsed_ms == 15000, "Production dog must back off for only 5 and 10 seconds");
    Require(app.device_state_ == kDeviceStateFatalError, "Production dog must retain fatal error state");
    Require(!app.protocol_initialized, "Production dog must not initialize a protocol after failure");
    Require(app.alert_statuses.back() == "Wi-Fi已连接，OTA服务不可用", "Explain OTA failure separately from Wi-Fi");
    Require(app.alert_messages.back().find("code=-23") != std::string::npos, "Show last OTA error code");
    Require(app.alert_messages.back().find("长按BOOT 5秒并松开可重新配网") != std::string::npos, "Show recovery gesture");
#else
    Require(calls == 10 && elapsed_ms == 5110000, "Preserve ten requests and original backoff for other profiles");
    Require(app.device_state_ != kDeviceStateFatalError, "Do not change other profiles' error state policy");
    Require(app.protocol_initialized, "Preserve other profiles' protocol fallback");
#endif
    ++tests;
}
void TestSuccess() {
    Seed({ESP_OK});
    Application app;
    app.Start();
    Require(calls == 1 && elapsed_ms == 0, "First successful response must proceed without delay");
    Require(app.protocol_initialized && marked_valid == 1, "Success must continue normal startup");
    Require(completed_events == MAIN_EVENT_CHECK_NEW_VERSION_DONE, "Success must signal completion");
    Require(app.device_state_ != kDeviceStateFatalError && app.alert_messages.empty(), "Success must not show failure");
    ++tests;
}
void TestTransientFailure() {
    Seed({-11, -12, ESP_OK});
    Application app;
    app.Start();
    Require(calls == 3 && marked_valid == 1, "Third successful request must be accepted");
    Require(app.protocol_initialized && app.device_state_ != kDeviceStateFatalError, "Retry success must not be fatal");
    Require(completed_events == MAIN_EVENT_CHECK_NEW_VERSION_DONE, "Retry success must signal completion");
#if defined(CONFIG_BOARD_TYPE_ZFSN_ESP32S3_DOG)
    Require(elapsed_ms == 15000, "Production retry success must use 5/10-second backoff");
#else
    Require(elapsed_ms == 30000, "Other profiles must use 10/20-second backoff");
#endif
    Require(app.alert_messages.size() == 2, "Only failed attempts may show retry alerts");
    ++tests;
}
void TestSecondAttemptSuccess() {
    Seed({-11, ESP_OK});
    Application app;
    app.Start();
    Require(calls == 2 && app.protocol_initialized, "Second attempt success must stop retrying");
    Require(app.device_state_ != kDeviceStateFatalError, "Transient error must not persist as fatal");
    ++tests;
}
}
int main() {
    try {
        TestFailureBudget();
        TestSuccess();
        TestTransientFailure();
        TestSecondAttemptSuccess();
        std::cout << tests << " native behavior tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
