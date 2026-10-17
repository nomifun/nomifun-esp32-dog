#include "display_board.h"
#include "application.h"
#include "assets.h"
#include "audio_codec.h"
#include "board.h"
#include "lvgl_theme.h"
#include "settings.h"
#ifdef CONFIG_ZFSN_UI_CAPTURE
#include "panel_capture.h"
#endif
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <font_awesome.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "Sh1106Display"

namespace {
// Emotion names must match the assets image's index.json.
constexpr const char* kEmotions[] = {
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying",
    "loving", "embarrassed", "surprised", "shocked", "thinking", "winking",
    "cool", "relaxed", "delicious", "kissy", "confident", "sleepy", "silly", "confused"
};
constexpr const char* kEmotionLabels[] = {
    "中性", "开心", "大笑", "搞笑", "悲伤", "愤怒", "哭泣", "爱心", "尴尬", "惊讶",
    "震惊", "思考", "眨眼", "酷", "放松", "美味", "亲吻", "自信", "困倦", "傻笑", "困惑"
};
constexpr const char* kApps[] = {"聊天", "表情", "音乐", "闹钟", "设置", "关于"};
constexpr const char* kSettings[] = {
    "音量设置", "开机动画", "充电灯效", "自由探索",
    "重新配网",
};
constexpr const char* kSettingKeys[] = {"", "boot_animation", "charging_led", "free_explore"};
constexpr int kResetNetworkSetting = 4;
constexpr const char* kMusicOptions[] = {"播放/停止", "下一首", "上一首", "停止播放"};
constexpr const char* kMusicActions[] = {"music_toggle", "music_next", "music_prev", "music_stop"};
constexpr int kHeaderHeight = 18;

std::string NormalizeEmotion(const char* value) {
    if (!value || !value[0]) return "neutral";
    std::string name(value);
    struct Alias { const char* from; const char* to; };
    constexpr Alias aliases[] = {
        {"happiness", "happy"}, {"sadness", "sad"}, {"asleep", "sleepy"},
        {"sleeping", "sleepy"}, {"close_eye", "sleepy"}, {"openeyes", "neutral"},
        {"surprise", "surprised"}, {"amazed", "surprised"}, {"scared", "shocked"},
        {"horrified", "shocked"}, {"furious", "angry"}, {"confuse", "confused"},
        {"skeptical", "confused"}, {"quirky", "funny"}, {"serious", "neutral"},
        {"aggrieved", "sad"}, {"pitiful", "sad"}, {"vulnerable", "sad"},
        {"weakness", "sad"}, {"guilty", "embarrassed"}, {"pleading", "sad"},
        {"contempt", "cool"}, {"comtempt", "cool"}, {"disgust", "angry"}, {"obscene", "silly"}
    };
    for (const auto& alias : aliases) if (name == alias.from) return alias.to;
    return name;
}

bool SafeName(const std::string& name) {
    return !name.empty() && name.size() <= 100 && name != "." && name != ".." &&
           name.find_first_of("/\\:\r\n") == std::string::npos && name.find('\0') == std::string::npos;
}

// The upstream gifdec API takes a pointer without a length. Validate all block
// boundaries before an SD file reaches it; reject impossible frame dimensions.
bool ValidGif(const std::vector<uint8_t>& data) {
    if (data.size() < 14 || (memcmp(data.data(), "GIF89a", 6) && memcmp(data.data(), "GIF87a", 6))) return false;
    auto word = [&](size_t p) { return unsigned(data[p]) | (unsigned(data[p + 1]) << 8); };
    const unsigned width = word(6), height = word(8);
    if (!width || !height || width > 512 || height > 512 || !(data[10] & 0x80)) return false;
    size_t pos = 13;
    if (data[10] & 0x80) pos += 3U << ((data[10] & 7) + 1);
    bool frame = false;
    while (pos < data.size()) {
        const uint8_t block = data[pos++];
        if (block == 0x3b) return frame;
        if (block == 0x21) {
            if (pos >= data.size()) return false;
            uint8_t label = data[pos++];
            if (pos >= data.size()) return false;
            // gifdec reads these fixed-size headers directly.
            if (label == 0xf9) {
                if (pos + 6 > data.size() || data[pos] != 4 || data[pos + 5] != 0) return false;
                pos += 6;
                continue;
            }
            if ((label == 0xff && data[pos] != 11) || (label == 0x01 && data[pos] != 12)) return false;
            if (label != 0xff && label != 0x01 && label != 0xfe) return false;
            if (label == 0xff && pos + 12 <= data.size() &&
                memcmp(data.data() + pos + 1, "NETSCAPE", 8) == 0) {
                // gifdec reads the Netscape loop extension as a fixed block.
                // A generic sub-block walk would accept an empty/truncated one.
                if (pos + 17 > data.size() || data[pos + 12] != 3 ||
                    data[pos + 13] != 1 || data[pos + 16] != 0) return false;
            }
        } else if (block == 0x2c) {
            if (pos + 9 > data.size()) return false;
            unsigned x = word(pos), y = word(pos + 2), w = word(pos + 4), h = word(pos + 6);
            if (!w || !h || x + w > width || y + h > height) return false;
            uint8_t packed = data[pos + 8];
            pos += 9;
            if (packed & 0x80) pos += 3U << ((packed & 7) + 1);
            if (pos >= data.size() || data[pos] < 2 || data[pos] > 8) return false;
            ++pos;
            frame = true;
        } else return false;
        while (true) {
            if (pos >= data.size()) return false;
            size_t count = data[pos++];
            if (count > data.size() - pos) return false;
            pos += count;
            if (!count) break;
        }
    }
    return false;
}

void PlainObject(lv_obj_t* object) {
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void SetLabelText(lv_obj_t* label, const std::string& text) {
    // Keeping an unchanged label avoids allocation and preserves its scroll.
    auto update = [label](const char* value) {
        if (strcmp(lv_label_get_text(label), value) == 0) return;
        // A different field/song starts at its first character. LVGL normally
        // carries a previous label's animation position into replacement text.
        lv_label_set_long_mode(label, lv_label_get_long_mode(label));
        lv_label_set_text(label, value);
    };
    if (text.find_first_of("\r\n\t") == std::string::npos) {
        update(text.c_str());
        return;
    }
    std::string single_line = text;
    std::replace_if(single_line.begin(), single_line.end(), [](char c) { return c == '\r' || c == '\n' || c == '\t'; }, ' ');
    update(single_line.c_str());
}
}

Sh1106Display::Sh1106Display(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, bool mirror_x, bool mirror_y)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y) {
    if (!display_) return;
    SetupDogUI();
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { static_cast<Sh1106Display*>(arg)->Tick(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "chat_time_update",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &time_update_timer_) == ESP_OK) {
        esp_timer_start_periodic(time_update_timer_, 1000000);
    }
    ESP_LOGI(TAG, "UI ready (menu, 21 PNG emotions)");
}

bool Sh1106Display::VolumePageActive() {
    DisplayLockGuard lock(this);
    return menu_active_.load() && page_ == Page::Volume;
}

Sh1106Display::~Sh1106Display() {
    if (time_update_timer_) {
        esp_timer_stop(time_update_timer_);
        esp_timer_delete(time_update_timer_);
    }
    DisplayLockGuard lock(this);
    if (firework_timer_) lv_timer_delete(firework_timer_);
    ClearImageLocked();
    lv_image_cache_drop(&menu_icon_dsc_);
    if (body_) lv_obj_delete(body_);
    if (header_) lv_obj_delete(header_);
    network_label_ = mute_label_ = battery_label_ = nullptr;
    status_label_ = notification_label_ = nullptr;
    if (display_ && current_theme_) {
        auto font = static_cast<LvglTheme*>(current_theme_)->text_font();
        if (font) lv_obj_set_style_text_font(lv_display_get_screen_active(display_), font->font(), 0);
    }
}

void Sh1106Display::LoadMenuFontLocked() {
    void* data = nullptr;
    size_t size = 0;
    if (!Assets::GetInstance().GetAssetData("font_puhui_common_14_1.bin", data, size) || size < 36) {
        ESP_LOGE(TAG, "Early menu font unavailable; built-in fallback may miss Chinese glyphs");
        return;
    }
    auto font = std::make_shared<LvglCBinFont>(data);
    if (!font->font() || font->font()->line_height != 18) {
        ESP_LOGE(TAG, "Early menu font rejected: expected 18px line height");
        return;
    }
    constexpr char32_t required[] = U"聊天表情音乐闹钟设置关于音量调节开机动画充电灯效自由探索重新配网开启关闭取消清除并启确认返回上级退出菜单中键仅供查看时间歌曲状态固件版本网络地址设备型号左右切换进入选择显示保留读取未文件停止下一首上一中性开心大笑搞笑悲伤愤怒哭泣爱心尴尬惊讶震惊思考眨眼酷放松美味亲吻自信困倦傻笑困惑说明";
    int missing = 0;
    for (char32_t codepoint : required) {
        if (!codepoint) break;
        lv_font_glyph_dsc_t glyph = {};
        if (!lv_font_get_glyph_dsc(font->font(), &glyph, uint32_t(codepoint), 0) || glyph.is_placeholder) ++missing;
    }
    if (missing) {
        ESP_LOGE(TAG, "Early menu font rejected: %d required glyphs missing", missing);
        return;
    }
    // Own this font independently of Assets::Apply replacing the theme later.
    menu_font_ = std::move(font);
    static_cast<LvglTheme*>(current_theme_)->set_text_font(menu_font_);
    lv_obj_set_style_text_font(lv_display_get_screen_active(display_), menu_font_->font(), 0);
    ESP_LOGI(TAG, "Early menu font ready: %u bytes, line_height=%d, checked=%u, missing=%d", unsigned(size), int(menu_font_->font()->line_height), unsigned(std::size(required) - 1), missing);
}

void Sh1106Display::SetupDogUI() {
    DisplayLockGuard lock(this);
    LoadMenuFontLocked();
    auto screen = lv_display_get_screen_active(display_);
    // The base OLED layout reserves 16px for an 18px font and overlays full-
    // width text on its icons. Reuse its live labels in explicit, separate slots.
    header_ = lv_obj_create(screen);
    PlainObject(header_);
    lv_obj_set_size(header_, width_, kHeaderHeight);
    lv_obj_set_pos(header_, 0, 0);
    lv_obj_set_style_bg_color(header_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(header_, LV_OPA_COVER, 0);
    for (auto label : {network_label_, mute_label_, battery_label_, status_label_, notification_label_}) {
        lv_obj_set_parent(label, header_);
        lv_obj_set_height(label, kHeaderHeight);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(network_label_, 18);
    lv_obj_align(mute_label_, LV_ALIGN_TOP_LEFT, width_ - 34, 0);
    lv_obj_set_width(mute_label_, 16);
    lv_obj_align(battery_label_, LV_ALIGN_TOP_LEFT, width_ - 18, 0);
    lv_obj_set_width(battery_label_, 18);
    for (auto label : {status_label_, notification_label_}) {
        // The base class centered these labels. Reset alignment explicitly;
        // set_pos alone would retain CENTER after reparenting.
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 18, 0);
        lv_obj_set_width(label, width_ - 52);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }

    body_ = lv_obj_create(screen);
    PlainObject(body_);
    lv_obj_set_pos(body_, 0, kHeaderHeight);
    lv_obj_set_size(body_, width_, height_ - kHeaderHeight);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(body_, lv_color_white(), 0);
    lv_obj_set_style_text_color(body_, lv_color_black(), 0);
    image_ = lv_image_create(body_);
    fallback_ = lv_label_create(body_);
    lv_obj_set_style_text_font(fallback_, static_cast<LvglTheme*>(current_theme_)->large_icon_font()->font(), 0);
    subtitle_ = lv_label_create(body_);
    lv_label_set_long_mode(subtitle_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(subtitle_, "");
    menu_ = lv_obj_create(body_);
    PlainObject(menu_);
    lv_obj_set_size(menu_, width_, height_);
    lv_obj_set_style_bg_color(menu_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(menu_, LV_OPA_COVER, 0);
    menu_title_ = lv_label_create(menu_);
    menu_counter_ = lv_label_create(menu_);
    menu_icon_ = lv_image_create(menu_);
    menu_left_ = lv_label_create(menu_);
    menu_right_ = lv_label_create(menu_);
    menu_value_ = lv_label_create(menu_);
    menu_info_ = lv_label_create(menu_);
    for (auto label : {menu_title_, menu_counter_, menu_value_, menu_info_, menu_left_, menu_right_}) {
        lv_obj_set_height(label, 18);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    }
    lv_obj_set_pos(menu_title_, 4, 0); lv_obj_set_width(menu_title_, 76);
    lv_obj_set_pos(menu_counter_, 84, 0); lv_obj_set_width(menu_counter_, 40);
    lv_obj_set_style_text_align(menu_counter_, LV_TEXT_ALIGN_RIGHT, 0);
    // The arrows point to the framed option, on its text's exact line. The
    // separate caption below is information, never another selectable option.
    lv_obj_set_pos(menu_value_, 43, 22); lv_obj_set_size(menu_value_, 72, 24);
    lv_obj_set_style_border_width(menu_value_, 1, 0);
    lv_obj_set_style_border_side(menu_value_, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_border_opa(menu_value_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menu_value_, lv_color_black(), 0);
    lv_obj_set_style_radius(menu_value_, 0, 0);
    lv_obj_set_style_pad_left(menu_value_, 0, 0);
    lv_obj_set_style_pad_right(menu_value_, 0, 0);
    lv_obj_set_style_pad_top(menu_value_, 2, 0);
    lv_obj_set_style_pad_bottom(menu_value_, 2, 0);
    lv_label_set_long_mode(menu_value_, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(menu_info_, 4, 46); lv_obj_set_width(menu_info_, 120);
    lv_obj_set_style_text_align(menu_value_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(menu_info_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(menu_icon_, 14, 22); lv_obj_set_size(menu_icon_, 24, 24);
    lv_obj_set_pos(menu_left_, 0, 25); lv_obj_set_width(menu_left_, 9); lv_label_set_text(menu_left_, "<");
    lv_obj_set_pos(menu_right_, 119, 25); lv_obj_set_width(menu_right_, 9); lv_label_set_text(menu_right_, ">");
    const uint32_t icon_colors[] = {0xffffffff, 0xff000000};
    memcpy(menu_icon_data_.data(), icon_colors, sizeof(icon_colors));
    menu_icon_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    menu_icon_dsc_.header.cf = LV_COLOR_FORMAT_I1;
    menu_icon_dsc_.header.w = 24;
    menu_icon_dsc_.header.h = 24;
    menu_icon_dsc_.header.stride = 3;
    menu_icon_dsc_.data_size = menu_icon_data_.size();
    menu_icon_dsc_.data = menu_icon_data_.data();
    lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
    const uint32_t colors[] = {0xff000000, 0xffffffff};
    memcpy(bitmap_.data(), colors, sizeof(colors));
    bitmap_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    bitmap_dsc_.header.cf = LV_COLOR_FORMAT_I1;
    bitmap_dsc_.header.w = 128;
    bitmap_dsc_.header.h = 64;
    bitmap_dsc_.header.stride = 16;
    bitmap_dsc_.data_size = bitmap_.size();
    bitmap_dsc_.data = bitmap_.data();
    SetEmotionLocked(emotion_);
    if (low_battery_popup_) lv_obj_move_foreground(low_battery_popup_);
}

void Sh1106Display::ClearImageLocked() {
    if (image_) lv_image_set_src(image_, nullptr);
    if (gif_ && gif_->image_dsc()) lv_image_cache_drop(gif_->image_dsc());
    gif_.reset();
    if (raw_image_) lv_image_cache_drop(raw_image_->image_dsc());
    raw_image_.reset();
}

void Sh1106Display::FitImageLocked(const lv_image_dsc_t* dsc, int width, int height) {
    lv_image_header_t header = dsc->header;
    if ((!header.w || !header.h) && lv_image_decoder_get_info(dsc, &header) != LV_RESULT_OK) return;
    if (!header.w || !header.h) return;
    lv_obj_set_size(image_, width, height);
    lv_image_set_inner_align(image_, LV_IMAGE_ALIGN_CONTAIN);
}

bool Sh1106Display::ShowRawImageLocked(void* data, size_t size) {
    ClearImageLocked();
    raw_image_ = std::make_unique<LvglRawImage>(data, size);
    if (size >= 6 && raw_image_->IsGif()) {
        gif_ = std::make_unique<LvglGif>(raw_image_->image_dsc());
        if (!gif_->IsLoaded()) { ClearImageLocked(); return false; }
        gif_->SetFrameCallback([this]() {
            if (image_ && gif_) {
                lv_image_set_src(image_, gif_->image_dsc());
                lv_obj_invalidate(image_);
            }
        });
        lv_image_set_src(image_, gif_->image_dsc());
        gif_->Start();
        if (menu_active_.load()) gif_->Pause();
    } else {
        lv_image_header_t header;
        if (lv_image_decoder_get_info(raw_image_->image_dsc(), &header) != LV_RESULT_OK) {
            ClearImageLocked(); return false;
        }
        lv_image_set_src(image_, raw_image_->image_dsc());
    }
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fallback_, LV_OBJ_FLAG_HIDDEN);
    return true;
}

void Sh1106Display::SetEmotionLocked(const std::string& emotion) {
    if (!body_) return;
    firework_active_ = false;
    if (firework_timer_) lv_timer_pause(firework_timer_);
    bitmap_active_ = false;
    emotion_ = emotion;
    void* data = nullptr;
    size_t size = 0;
    auto& assets = Assets::GetInstance();
    bool found = assets.GetAssetData(emotion + ".png", data, size);
    if (!found) found = assets.GetAssetData(emotion + ".gif", data, size);
    if (!found || !ShowRawImageLocked(data, size)) {
        ClearImageLocked();
        const char* glyph = font_awesome_get_utf8(emotion.c_str());
        lv_label_set_text(fallback_, glyph ? glyph : FONT_AWESOME_NEUTRAL);
        lv_obj_remove_flag(fallback_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(image_, LV_OBJ_FLAG_HIDDEN);
    }
    custom_data_.clear();
    LayoutChatLocked();
}

void Sh1106Display::LayoutChatLocked() {
    if (!body_) return;
    const bool subtitles = !chat_text_.empty() && !bitmap_active_ && !firework_active_;
    if (subtitles) {
        lv_obj_remove_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(subtitle_, width_ - 36, LV_SIZE_CONTENT);
        lv_obj_align(subtitle_, LV_ALIGN_RIGHT_MID, 0, 0);
    } else lv_obj_add_flag(subtitle_, LV_OBJ_FLAG_HIDDEN);
    const lv_image_dsc_t* dsc = bitmap_active_ || firework_active_ ? &bitmap_dsc_ :
        (gif_ ? gif_->image_dsc() : (raw_image_ ? raw_image_->image_dsc() : nullptr));
    if (dsc) FitImageLocked(dsc, subtitles ? 32 : width_, height_ - kHeaderHeight);
    // CONTAIN centres the source in the viewport using the source dimensions;
    // keep a non-square bitmap's aspect ratio and fit 64x64 PNGs below the bar.
    lv_obj_align(image_, subtitles ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(fallback_, subtitles ? LV_ALIGN_LEFT_MID : LV_ALIGN_CENTER, 0, 0);
}

void Sh1106Display::SetEmotion(const char* emotion) {
    std::string name = NormalizeEmotion(emotion);
    if (name == "fireworks" || name == "celebration") { PlayFirework(); return; }
    DisplayLockGuard lock(this);
    SetEmotionLocked(name);
}

bool Sh1106Display::RandomEmotion() {
    SetEmotion(kEmotions[esp_random() % std::size(kEmotions)]);
    return body_ != nullptr;
}

bool Sh1106Display::LoadCustomEmotion(const std::string& name) {
    if (!SafeName(name)) return false;
    std::string filename = name;
    bool bitmap = filename.size() > 4 && filename.substr(filename.size() - 4) == ".bin";
    if (!bitmap && (filename.size() <= 4 || filename.substr(filename.size() - 4) != ".gif")) filename += ".gif";
    std::string path = "/sdcard/custom_emo/" + filename;
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return false; }
    long length = ftell(file);
    if (length < 1 || length > 1024 * 1024 || fseek(file, 0, SEEK_SET)) { fclose(file); return false; }
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    bool read = fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    fclose(file);
    if (!read) return false;
    if (bitmap) return SetBitmap(bytes.data(), bytes.size());
    if (!ValidGif(bytes)) { ESP_LOGW(TAG, "Invalid SD emotion gif: %s", path.c_str()); return false; }
    DisplayLockGuard lock(this);
    if (!body_) return false;
    ClearImageLocked();
    custom_data_ = std::move(bytes);
    if (!ShowRawImageLocked(custom_data_.data(), custom_data_.size())) { SetEmotionLocked(emotion_); return false; }
    firework_active_ = bitmap_active_ = false;
    if (firework_timer_) lv_timer_pause(firework_timer_);
    ShowPageLocked(Page::Chat);
    LayoutChatLocked();
    ESP_LOGI(TAG, "Loaded SD emotion %s (%u bytes)", path.c_str(), unsigned(custom_data_.size()));
    return true;
}

bool Sh1106Display::SetBitmap(const uint8_t* bitmap, size_t size) {
    if (!bitmap || size != 1024) return false;
    DisplayLockGuard lock(this);
    if (!body_) return false;
    ClearImageLocked();
    custom_data_.clear();
    firework_active_ = false;
    if (firework_timer_) lv_timer_pause(firework_timer_);
    memcpy(bitmap_.data() + 8, bitmap, size);
    bitmap_active_ = true;
    ShowPageLocked(Page::Chat);
    ShowBitmapLocked();
    return true;
}

void Sh1106Display::ShowBitmapLocked() {
    lv_image_cache_drop(&bitmap_dsc_);
    lv_image_set_src(image_, &bitmap_dsc_);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(fallback_, LV_OBJ_FLAG_HIDDEN);
    LayoutChatLocked();
    lv_obj_invalidate(image_);
}

void Sh1106Display::PlayFirework() {
    DisplayLockGuard lock(this);
    if (!body_) return;
    ClearImageLocked();
    custom_data_.clear();
    bitmap_active_ = false;
    firework_active_ = true;
    firework_frame_ = 0;
    ShowPageLocked(Page::Chat);
    if (!firework_timer_) firework_timer_ = lv_timer_create([](lv_timer_t* timer) {
        static_cast<Sh1106Display*>(lv_timer_get_user_data(timer))->AdvanceFirework();
    }, 40, this);
    lv_timer_resume(firework_timer_);
    AdvanceFirework();
}

void Sh1106Display::AdvanceFirework() {
    if (!firework_active_) return;
    if (firework_frame_ >= 96) { SetEmotionLocked(emotion_); return; }
    memset(bitmap_.data() + 8, 0, 1024);
    int frame = firework_frame_++ % 24;
    if (frame == 0) { firework_x_ = 24 + esp_random() % 80; firework_y_ = 12 + esp_random() % 26; }
    auto pixel = [this](int x, int y) {
        if (x >= 0 && x < 128 && y >= 0 && y < 64) bitmap_[8 + y * 16 + x / 8] |= 0x80 >> (x % 8);
    };
    if (frame < 8) {
        int y = 63 - (63 - firework_y_) * frame / 8;
        for (int trail = 0; trail < 5; ++trail) pixel(firework_x_, y + trail);
    } else {
        constexpr int dx[] = {10,9,7,4,0,-4,-7,-9,-10,-9,-7,-4,0,4,7,9};
        constexpr int dy[] = {0,4,7,9,10,9,7,4,0,-4,-7,-9,-10,-9,-7,-4};
        int radius = (frame - 7) * 2;
        for (unsigned ray = 0; ray < std::size(dx); ++ray) for (int tail = 0; tail < 3; ++tail) {
            pixel(firework_x_ + dx[ray] * (radius - tail) / 10,
                  firework_y_ + dy[ray] * (radius - tail) / 10 + (frame - 8) / 4);
        }
    }
    ShowBitmapLocked();
}

void Sh1106Display::SetStatus(const char* status) {
    OledDisplay::SetStatus(status ? status : "");
}

void Sh1106Display::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (!body_) return;
    chat_text_ = content ? content : "";
    std::replace(chat_text_.begin(), chat_text_.end(), '\n', ' ');
    SetLabelText(subtitle_, chat_text_);
    LayoutChatLocked();
}

void Sh1106Display::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Sh1106Display::ShowNotification(const char* notification, int duration_ms) {
    std::string message = notification ? notification : "";
    std::replace_if(message.begin(), message.end(), [](char c) { return c == '\r' || c == '\n' || c == '\t'; }, ' ');
    OledDisplay::ShowNotification(message.c_str(), std::max(duration_ms, 1));
    DisplayLockGuard lock(this);
    notification_text_ = std::move(message);
    notification_until_ = esp_timer_get_time() + int64_t(std::max(duration_ms, 1)) * 1000;
    if (body_ && menu_active_.load()) RenderMenuLocked();
}

void Sh1106Display::SetTheme(Theme* theme) {
    OledDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    if (!body_ || !lvgl_theme || !lvgl_theme->text_font()) return;
    const auto font = menu_font_ ? menu_font_ : lvgl_theme->text_font();
    lv_obj_set_style_text_font(header_, font->font(), 0);
    lv_obj_set_style_text_font(body_, font->font(), 0);
    if (!gif_ && !bitmap_active_ && !firework_active_) SetEmotionLocked(emotion_);
}

void Sh1106Display::SetMenuIconLocked(const DogPixelIcons::Bitmap& bitmap) {
    if (memcmp(menu_icon_data_.data() + 8, bitmap.pixels.data(), bitmap.pixels.size()) == 0) return;
    lv_image_cache_drop(&menu_icon_dsc_);
    memcpy(menu_icon_data_.data() + 8, bitmap.pixels.data(), bitmap.pixels.size());
    lv_image_set_src(menu_icon_, &menu_icon_dsc_);
    lv_obj_invalidate(menu_icon_);
}

void Sh1106Display::SetMenuCallbacks(std::function<void(const std::string&)> action,
                                    std::function<std::string(const std::string&)> info) {
    DisplayLockGuard lock(this);
    action_callback_ = std::move(action);
    info_callback_ = std::move(info);
}

void Sh1106Display::OnSettingChanged(std::function<void(const std::string&, bool)> callback) {
    DisplayLockGuard lock(this);
    setting_callback_ = std::move(callback);
}

int Sh1106Display::PageItemCountLocked(Page page) const {
    switch (page) {
    case Page::Apps: return int(std::size(kApps)) + 1;
    case Page::Emotion: return int(std::size(kEmotions)) + 1;
    case Page::Music: return int(std::size(kMusicOptions)) + 1;
    case Page::Settings: return int(std::size(kSettings)) + 1;
    case Page::Alarm:
    case Page::About: return 4;
    case Page::ResetNetworkConfirm: return 2;
    default: return 1;
    }
}

bool Sh1106Display::IsBackItemLocked() const {
    switch (page_) {
    case Page::Apps:
    case Page::Emotion:
    case Page::Music:
    case Page::Alarm:
    case Page::Settings:
    case Page::About: return selection_ == PageItemCountLocked() - 1;
    default: return false;
    }
}

void Sh1106Display::BackLocked() {
    if (page_ == Page::Chat || page_ == Page::Apps) ShowPageLocked(Page::Chat);
    else if (page_ == Page::Volume) ShowPageLocked(Page::Settings);
    else if (page_ == Page::ResetNetworkConfirm) {
        page_selections_[static_cast<size_t>(Page::Settings)] = kResetNetworkSetting;
        ShowPageLocked(Page::Settings);
    }
    else ShowPageLocked(Page::Apps);
}

void Sh1106Display::ShowPageLocked(Page page) {
    // A Back/Exit item must not become the next entry's default selection.
    if (!IsBackItemLocked()) page_selections_[static_cast<size_t>(page_)] = selection_;
    page_ = page;
    // Destructive confirmation always starts at Cancel, even after a previous
    // visit. Other pages remember their own selection and their parent's place.
    selection_ = page == Page::ResetNetworkConfirm ? 0 :
        std::clamp(page_selections_[static_cast<size_t>(page)], 0, PageItemCountLocked(page) - 1);
    menu_active_ = page != Page::Chat;
    if (page == Page::Chat) {
        lv_obj_add_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(body_, 0, kHeaderHeight);
        lv_obj_set_size(body_, width_, height_ - kHeaderHeight);
        lv_obj_remove_flag(header_, LV_OBJ_FLAG_HIDDEN);
        if (gif_) gif_->Resume();
        if (firework_active_ && firework_timer_) lv_timer_resume(firework_timer_);
    } else {
        if (gif_) gif_->Pause();
        if (firework_timer_) lv_timer_pause(firework_timer_);
        lv_obj_add_flag(header_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(body_, 0, 0);
        lv_obj_set_size(body_, width_, height_);
        lv_obj_remove_flag(menu_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(menu_);
        RenderMenuLocked();
    }
}

std::string Sh1106Display::InfoKeyLocked() const {
    if (IsBackItemLocked()) return "";
    if (page_ == Page::Music) return "music";
    if (page_ == Page::Alarm) {
        constexpr const char* keys[] = {"alarm_time", "alarm_music", "alarm_state"};
        return keys[selection_];
    }
    if (page_ == Page::About) {
        constexpr const char* keys[] = {"about_version", "about_network", "about_board"};
        return keys[selection_];
    }
    return "";
}

void Sh1106Display::RenderMenuLocked() {
    if (!body_ || page_ == Page::Chat) return;
    char title[64];
    const int count = PageItemCountLocked();
    std::string value, info;
    using Icon = DogPixelIcons::Icon;
    Icon icon = Icon::Chat;
    const bool back_item = IsBackItemLocked();
    if (back_item) {
        const char* category = page_ == Page::Apps ? "应用" : page_ == Page::Emotion ? "表情" :
            page_ == Page::Music ? "音乐" : page_ == Page::Alarm ? "闹钟" : page_ == Page::Settings ? "设置" : "关于";
        snprintf(title, sizeof(title), "%s", category);
        icon = Icon::Back;
        value = page_ == Page::Apps ? "退出菜单" : "返回上级";
        info = page_ == Page::Apps ? "返回聊天" : "返回应用";
    } else switch (page_) {
    case Page::Apps: {
        constexpr Icon icons[] = {Icon::Chat, Icon::Emotion, Icon::Music, Icon::Alarm, Icon::Settings, Icon::About};
        snprintf(title, sizeof(title), "应用");
        icon = icons[selection_]; value = kApps[selection_]; info = "确认进入"; break;
    }
    case Page::Emotion:
        snprintf(title, sizeof(title), "表情");
        icon = Icon::Emotion; value = kEmotionLabels[selection_]; info = "确认显示"; break;
    case Page::Music: {
        constexpr Icon icons[] = {Icon::Play, Icon::Next, Icon::Previous, Icon::Stop};
        snprintf(title, sizeof(title), "音乐");
        icon = icons[selection_]; value = kMusicOptions[selection_];
        info = menu_info_text_.empty() ? "无音乐文件" : menu_info_text_; break;
    }
    case Page::Alarm:
        snprintf(title, sizeof(title), "闹钟");
        icon = selection_ == 0 ? Icon::Calendar : selection_ == 1 ? Icon::Music : Icon::Toggle;
        value = selection_ == 0 ? "时间" : selection_ == 1 ? "歌曲" : "闹钟开关";
        info = menu_info_text_.empty() ? "未设置" : menu_info_text_; break;
    case Page::Settings: {
        constexpr Icon icons[] = {Icon::Volume, Icon::Boot, Icon::Charging, Icon::Explore, Icon::Network};
        snprintf(title, sizeof(title), "设置");
        icon = icons[selection_];
        value = kSettings[selection_];
        if (selection_ == 0) {
            auto codec = Board::GetInstance().GetAudioCodec();
            info = codec ? std::to_string(codec->output_volume()) + "%" : "--";
        } else if (selection_ < int(std::size(kSettingKeys))) {
            Settings settings("display");
            info = settings.GetBool(kSettingKeys[selection_], selection_ != 3) ? "开启" : "关闭";
        } else {
            info = "确认进入";
        }
        break;
    }
    case Page::ResetNetworkConfirm:
        snprintf(title, sizeof(title), "配网确认"); icon = Icon::Network;
        value = selection_ == 0 ? "取消" : "清除重启";
        info = selection_ == 0 ? "说明：保留网络" : "说明：清除网络";
        break;
    case Page::Volume: {
        snprintf(title, sizeof(title), "音量设置"); icon = Icon::Volume;
        auto codec = Board::GetInstance().GetAudioCodec();
        value = codec ? std::to_string(codec->output_volume()) + "%" : "--";
        info = "中键返回";
        break;
    }
    case Page::About:
        snprintf(title, sizeof(title), "关于");
        icon = selection_ == 0 ? Icon::Chip : selection_ == 1 ? Icon::Network : Icon::Dog;
        value = selection_ == 0 ? "固件版本" : selection_ == 1 ? "网络地址" : "设备型号";
        info = menu_info_text_.empty() ? "读取中" : menu_info_text_;
        break;
    default: return;
    }
    SetLabelText(menu_title_, title);
    char counter[16] = "";
    if (count > 1) snprintf(counter, sizeof(counter), "%d/%d", selection_ + 1, count);
    SetLabelText(menu_counter_, counter);
    SetMenuIconLocked(page_ == Page::Emotion && !back_item ? DogPixelIcons::kEmotions[selection_] : DogPixelIcons::kIcons[static_cast<size_t>(icon)]);
    SetLabelText(menu_value_, value);
    if (notification_until_ > esp_timer_get_time() && !notification_text_.empty()) info = notification_text_;
    SetLabelText(menu_info_, info);
}

void Sh1106Display::MoveMenu(int direction) {
    {
        DisplayLockGuard lock(this);
        if (!body_) return;
        if (page_ == Page::Chat) { ShowPageLocked(Page::Apps); return; }
        const auto old_key = InfoKeyLocked();
        const int count = PageItemCountLocked();
        if (page_ == Page::Volume) {
            auto codec = Board::GetInstance().GetAudioCodec();
            if (codec) codec->SetOutputVolume(std::clamp(codec->output_volume() + direction * 5, 0, 100));
        } else selection_ = (selection_ + direction + count) % count;
        if (!IsBackItemLocked()) page_selections_[static_cast<size_t>(page_)] = selection_;
        if (old_key != InfoKeyLocked()) menu_info_text_.clear();
        RenderMenuLocked();
    }
    Tick();
}

void Sh1106Display::MenuNext() { MoveMenu(1); }
void Sh1106Display::MenuPrevious() { MoveMenu(-1); }

void Sh1106Display::MenuConfirm() {
    std::string action, setting, notification;
    bool enabled = false;
    std::function<void(const std::string&)> callback;
    std::function<void(const std::string&, bool)> setting_callback;
    {
        DisplayLockGuard lock(this);
        if (!body_) return;
        callback = action_callback_;
        setting_callback = setting_callback_;
        if (page_ == Page::Chat) ShowPageLocked(Page::Apps);
        else if (IsBackItemLocked()) BackLocked();
        else if (page_ == Page::Apps) {
            constexpr Page pages[] = {Page::Chat, Page::Emotion, Page::Music, Page::Alarm, Page::Settings, Page::About};
            auto target = pages[selection_];
            menu_info_text_.clear(); ShowPageLocked(target);
            if (target == Page::Chat) action = "chat";
        } else if (page_ == Page::Emotion) {
            SetEmotionLocked(kEmotions[selection_]); ShowPageLocked(Page::Chat);
        } else if (page_ == Page::Music) action = kMusicActions[selection_];
        else if (page_ == Page::Alarm) {
            if (selection_ == 2) action = "alarm_toggle";
            else notification = "仅供查看";
        }
        else if (page_ == Page::Settings) {
            if (selection_ == 0) ShowPageLocked(Page::Volume);
            else if (selection_ == kResetNetworkSetting) {
                // ShowPageLocked defaults to Cancel. A repeated confirmation
                // cannot erase configuration without selecting the reset option.
                ShowPageLocked(Page::ResetNetworkConfirm);
            }
            else {
                setting = kSettingKeys[selection_];
                { Settings settings("display", true);
                  enabled = !settings.GetBool(setting, selection_ != 3);
                  settings.SetBool(setting, enabled); }
                RenderMenuLocked();
            }
        }
        else if (page_ == Page::ResetNetworkConfirm) {
            if (selection_ == 1) action = "reset_network";
            BackLocked();
        }
        else if (page_ == Page::Volume || page_ == Page::About) BackLocked();
    }
    if (callback && !action.empty()) callback(action);
    if (setting_callback && !setting.empty()) setting_callback(setting, enabled);
    if (!notification.empty()) ShowNotification(notification);
    Tick();
}

void Sh1106Display::MenuBack() {
    DisplayLockGuard lock(this);
    if (!body_) return;
    BackLocked();
}

void Sh1106Display::ExitMenu() {
    DisplayLockGuard lock(this);
    if (body_) ShowPageLocked(Page::Chat);
}

#ifdef CONFIG_ZFSN_UI_CAPTURE
int Sh1106Display::UiCaptureSceneItemCount(int scene) const {
    if (scene < 0 || scene >= int(kUiCapturePages.size())) return 0;
    return PageItemCountLocked(kUiCapturePages[scene]);
}

bool Sh1106Display::ShowUiCaptureScene(int page, int selection, const char* capture_label) {
    if (selection < 0 || selection >= UiCaptureSceneItemCount(page)) return false;
    {
        DisplayLockGuard lock(this);
        if (!body_) return false;
        ShowPageLocked(kUiCapturePages[page]);
        selection_ = selection;
        menu_info_text_.clear();
        notification_until_ = 0;
        RenderMenuLocked();
    }
    // Read-only information is fetched through the same path as navigation.
    // This preview never invokes action_callback_ or setting_callback_.
    Tick();
    {
        DisplayLockGuard lock(this);
        if (RequestDogPanelFrame(capture_label) != ESP_OK) return false;
        lv_obj_invalidate(menu_);
        lv_obj_invalidate(body_);
    }
    return true;
}
#endif

void Sh1106Display::Tick() {
    std::function<std::string(const std::string&)> callback;
    std::string key;
    {
        DisplayLockGuard lock(this);
        if (!body_) return;
        callback = info_callback_;
        key = InfoKeyLocked();
    }
    // Media callbacks must run outside LVGL's lock.
    std::string info = callback && !key.empty() ? callback(key) : "";
    DisplayLockGuard lock(this);
    // A field may have changed while its service callback was running.
    if (InfoKeyLocked() != key) return;
    if (!key.empty()) menu_info_text_ = std::move(info);
    RenderMenuLocked();
    // The app's existing 1s status-bar update owns the idle clock and temporary
    // notification expiry, so those timings are preserved.
}
