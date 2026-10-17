#pragma once

#include <esp_err.h>
#include <esp_lcd_panel_ops.h>

// Diagnostic-only hook for this board's single 128x64 SH1106. Install before
// starting LVGL. With CONFIG_ZFSN_UI_CAPTURE disabled every function returns
// ESP_ERR_NOT_SUPPORTED and no framebuffer or hook is present.
esp_err_t InstallDogPanelCapture(esp_lcd_panel_handle_t panel);

// Capture the next successful full-screen driver submission. Call before
// invalidating/refreshing the desired scene while holding the display lock.
// A draw already in flight when requested is not eligible. Successful full-draw
// sequence and request baseline are included in each exported record.
// Labels are 1..48 ASCII characters
// from [A-Za-z0-9_-], so no runtime/user text enters the capture log protocol.
// A pending or unread request must be consumed before another request starts.
esp_err_t RequestDogPanelFrame(const char* label);

// Consume a captured frame and emit its bounded, CRC-protected hex record to
// the ESP log. Call from a diagnostic task, outside the LVGL/display lock.
// Returns ESP_ERR_NOT_FINISHED while a request awaits a full successful draw.
// No frame log is emitted from the display's draw/flush path.
esp_err_t DumpDogPanelFrame();
