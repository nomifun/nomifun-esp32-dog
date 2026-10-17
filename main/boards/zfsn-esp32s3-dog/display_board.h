#pragma once

#include "display/oled_display.h"
#include "gif/lvgl_gif.h"
#include "pixel_icons.h"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class LvglFont;

// Menu-driven OLED UI for this board. Menus use the full 128x64 canvas;
// notifications remain visible in their footer.
class Sh1106Display : public OledDisplay {
public:
    Sh1106Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, bool mirror_x, bool mirror_y);
    ~Sh1106Display();

    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    bool RandomEmotion();
    bool LoadCustomEmotion(const std::string& name);
    bool SetBitmap(const uint8_t* bitmap, size_t size);
    void PlayFirework();
    void MenuNext();
    void MenuPrevious();
    void MenuConfirm();
    void MenuBack();
    void ExitMenu();
    bool MenuActive() const { return menu_active_.load(); }
    bool VolumePageActive();
#ifdef CONFIG_ZFSN_UI_CAPTURE
    int UiCaptureSceneItemCount(int scene) const;
    bool ShowUiCaptureScene(int page, int selection, const char* capture_label);
#endif
    // music_toggle/music_next/music_prev/music_stop/alarm_toggle/chat;
    // production only: reset_network after explicit menu confirmation.
    // info requests: music/alarm/about. Services remain owned by the board.
    void SetMenuCallbacks(std::function<void(const std::string&)> action,
                          std::function<std::string(const std::string&)> info);
    void OnSettingChanged(std::function<void(const std::string&, bool)> callback);

private:
    enum class Page { Chat, Apps, Emotion, Music, Alarm, Settings, Volume, About, ResetNetworkConfirm };
#ifdef CONFIG_ZFSN_UI_CAPTURE
    static constexpr std::array<Page, 8> kUiCapturePages = {Page::Apps, Page::Settings, Page::Music, Page::Alarm, Page::About, Page::Volume, Page::ResetNetworkConfirm, Page::Emotion};
#endif
    esp_timer_handle_t time_update_timer_ = nullptr;
    lv_timer_t* firework_timer_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* fallback_ = nullptr;
    lv_obj_t* subtitle_ = nullptr;
    lv_obj_t* menu_ = nullptr;
    lv_obj_t* menu_title_ = nullptr;
    lv_obj_t* menu_counter_ = nullptr;
    lv_obj_t* menu_icon_ = nullptr;
    lv_obj_t* menu_left_ = nullptr;
    lv_obj_t* menu_right_ = nullptr;
    lv_obj_t* menu_value_ = nullptr;
    lv_obj_t* menu_info_ = nullptr;
    std::unique_ptr<LvglRawImage> raw_image_;
    std::unique_ptr<LvglGif> gif_;
    std::shared_ptr<LvglFont> menu_font_;
    lv_image_dsc_t menu_icon_dsc_ = {};
    std::array<uint8_t, 80> menu_icon_data_ = {};
    std::vector<uint8_t> custom_data_;
    lv_image_dsc_t bitmap_dsc_ = {};
    std::array<uint8_t, 1032> bitmap_ = {};
    bool bitmap_active_ = false;
    bool firework_active_ = false;
    int firework_frame_ = 0, firework_x_ = 64, firework_y_ = 24;
    std::string emotion_ = "neutral", chat_text_, menu_info_text_;
    std::string notification_text_;
    int64_t notification_until_ = 0;
    std::atomic<bool> menu_active_ = false;
    Page page_ = Page::Chat;
    int selection_ = 0;
    std::array<int, 9> page_selections_ = {};
    std::function<void(const std::string&)> action_callback_;
    std::function<std::string(const std::string&)> info_callback_;
    std::function<void(const std::string&, bool)> setting_callback_;

    void SetupDogUI();
    void LoadMenuFontLocked();
    void SetMenuIconLocked(const DogPixelIcons::Bitmap& bitmap);
    void Tick();
    void MoveMenu(int direction);
    int PageItemCountLocked(Page page) const;
    int PageItemCountLocked() const { return PageItemCountLocked(page_); }
    bool IsBackItemLocked() const;
    void BackLocked();
    void ShowPageLocked(Page page);
    void RenderMenuLocked();
    std::string InfoKeyLocked() const;
    void LayoutChatLocked();
    void SetEmotionLocked(const std::string& emotion);
    bool ShowRawImageLocked(void* data, size_t size);
    void ClearImageLocked();
    void FitImageLocked(const lv_image_dsc_t* image, int width, int height);
    void ShowBitmapLocked();
    void AdvanceFirework();
};
