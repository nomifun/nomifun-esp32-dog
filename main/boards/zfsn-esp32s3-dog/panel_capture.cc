#include "panel_capture.h"

#include "sdkconfig.h"

#if defined(CONFIG_ZFSN_UI_CAPTURE) && CONFIG_ZFSN_UI_CAPTURE

#include <esp_lcd_panel_interface.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <limits>

namespace {
constexpr char kTag[] = "DogPanelCapture";
constexpr size_t kWidth = 128;
constexpr size_t kHeight = 64;
constexpr size_t kFrameBytes = kWidth * kHeight / 8;
constexpr size_t kChunkBytes = 64;
constexpr size_t kChunks = kFrameBytes / kChunkBytes;
constexpr size_t kLabelMax = 48;
constexpr TickType_t kLockWait = pdMS_TO_TICKS(20);
using DrawBitmap = esp_err_t (*)(esp_lcd_panel_t*, int, int, int, int, const void*);

struct Frame {
    std::array<uint8_t, kFrameBytes> bytes{};
    std::array<char, kLabelMax + 1> label{};
    uint32_t id = 0;
    uint32_t sequence = 0;
    uint32_t request_after = 0;
};

esp_lcd_panel_handle_t panel_ = nullptr;
DrawBitmap original_draw_ = nullptr;
StaticSemaphore_t state_mutex_storage_{};
StaticSemaphore_t dump_mutex_storage_{};
SemaphoreHandle_t state_mutex_ = nullptr;
SemaphoreHandle_t dump_mutex_ = nullptr;
Frame frame_;
bool pending_ = false;
bool ready_ = false;
std::atomic<uint32_t> successful_full_draws_{0};

uint32_t Crc32(const uint8_t* data, size_t size) {
    // IEEE CRC-32, identical to Python's binascii.crc32(data).
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return ~crc;
}

esp_err_t CaptureDraw(esp_lcd_panel_t* panel, int x_start, int y_start,
                      int x_end, int y_end, const void* color_data) {
    const bool full_screen = color_data != nullptr && x_start == 0 && y_start == 0 &&
                             x_end == kWidth && y_end == kHeight;
    uint32_t request_at_draw_start = 0;
    if (full_screen && xSemaphoreTake(state_mutex_, 0) == pdTRUE) {
        request_at_draw_start = pending_ ? frame_.id : 0;
        xSemaphoreGive(state_mutex_);
    }
    // Keep the original call and return value unchanged, including failed and
    // partial draws. SH1106/LVGL's conversion has already produced page bytes.
    const esp_err_t result = original_draw_(panel, x_start, y_start,
                                            x_end, y_end, color_data);
    if (result == ESP_OK && full_screen) {
        const uint32_t sequence = successful_full_draws_.fetch_add(1) + 1;
        if (request_at_draw_start != 0 && xSemaphoreTake(state_mutex_, 0) == pdTRUE) {
            // An already in-flight old draw cannot satisfy a request made
            // during that draw. The request must predate its driver entry.
            if (pending_ && frame_.id == request_at_draw_start) {
                std::memcpy(frame_.bytes.data(), color_data, kFrameBytes);
                frame_.sequence = sequence;
                pending_ = false;
                ready_ = true;
            }
            xSemaphoreGive(state_mutex_);
        }
    }
    return result;
}
}  // namespace

esp_err_t InstallDogPanelCapture(esp_lcd_panel_handle_t panel) {
    if (panel == nullptr || panel->draw_bitmap == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    // Installation is a board startup operation, before LVGL can call draw.
    if (panel_ != nullptr) {
        return panel_ == panel && panel->draw_bitmap == CaptureDraw
                   ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    state_mutex_ = xSemaphoreCreateMutexStatic(&state_mutex_storage_);
    dump_mutex_ = xSemaphoreCreateMutexStatic(&dump_mutex_storage_);
    if (state_mutex_ == nullptr || dump_mutex_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    original_draw_ = panel->draw_bitmap;
    panel_ = panel;
    panel->draw_bitmap = CaptureDraw;
    return ESP_OK;
}

esp_err_t RequestDogPanelFrame(const char* label) {
    if (label == nullptr || label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = 0;
    while (length <= kLabelMax && label[length] != '\0') {
        const char c = label[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return ESP_ERR_INVALID_ARG;
        }
        ++length;
    }
    if (length > kLabelMax) {
        return ESP_ERR_INVALID_ARG;
    }
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(state_mutex_, kLockWait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (pending_ || ready_ || frame_.id == std::numeric_limits<uint32_t>::max()) {
        xSemaphoreGive(state_mutex_);
        return ESP_ERR_INVALID_STATE;
    }
    std::memcpy(frame_.label.data(), label, length + 1);
    ++frame_.id;
    frame_.request_after = successful_full_draws_.load();
    pending_ = true;
    xSemaphoreGive(state_mutex_);
    return ESP_OK;
}

esp_err_t DumpDogPanelFrame() {
    if (panel_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Serialize records without holding the state lock during USB logging.
    // The LVGL thread never waits for either mutex.
    if (xSemaphoreTake(dump_mutex_, kLockWait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(state_mutex_, kLockWait) != pdTRUE) {
        xSemaphoreGive(dump_mutex_);
        return ESP_ERR_TIMEOUT;
    }
    if (!ready_) {
        xSemaphoreGive(state_mutex_);
        xSemaphoreGive(dump_mutex_);
        return ESP_ERR_NOT_FINISHED;
    }
    const Frame snapshot = frame_;
    ready_ = false;
    xSemaphoreGive(state_mutex_);

    const uint32_t crc = Crc32(snapshot.bytes.data(), snapshot.bytes.size());
    ESP_LOGI(kTag, "DOG_PANEL_V1 BEGIN id=%" PRIu32 " label=%s width=128 height=64 "
                  "bytes=1024 chunks=16 crc32=%08" PRIx32 " seq=%" PRIu32 " after=%" PRIu32
                  " success=1 layout=sh1106-page-lsb polarity=1",
             snapshot.id, snapshot.label.data(), crc, snapshot.sequence, snapshot.request_after);
    constexpr char hex_digits[] = "0123456789abcdef";
    std::array<char, kChunkBytes * 2 + 1> hex{};
    for (size_t chunk = 0; chunk < kChunks; ++chunk) {
        for (size_t i = 0; i < kChunkBytes; ++i) {
            const uint8_t byte = snapshot.bytes[chunk * kChunkBytes + i];
            hex[i * 2] = hex_digits[byte >> 4];
            hex[i * 2 + 1] = hex_digits[byte & 0xf];
        }
        ESP_LOGI(kTag, "DOG_PANEL_V1 DATA id=%" PRIu32 " part=%u/16 hex=%s",
                 snapshot.id, static_cast<unsigned>(chunk), hex.data());
    }
    ESP_LOGI(kTag, "DOG_PANEL_V1 END id=%" PRIu32 " crc32=%08" PRIx32, snapshot.id, crc);
    xSemaphoreGive(dump_mutex_);
    return ESP_OK;
}

#else

esp_err_t InstallDogPanelCapture(esp_lcd_panel_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t RequestDogPanelFrame(const char*) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t DumpDogPanelFrame() { return ESP_ERR_NOT_SUPPORTED; }

#endif
