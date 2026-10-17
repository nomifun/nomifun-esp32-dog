#pragma once
#include <lvgl.h>
#include <esp_timer.h>
#include "lvgl_theme.h"
#include <string>
#include <cstdint>
#include <cstddef>
class DisplayLockGuard {
public:
    template<class T> explicit DisplayLockGuard(T*) {}
};
class LvglRawImage {
    lv_image_dsc_t dsc_{};
public:
    LvglRawImage(void*,size_t) {}
    bool IsGif() const { return false; }
    const lv_image_dsc_t* image_dsc() const { return &dsc_; }
};
class LvglDisplay {
protected:
    lv_display_t* display_=nullptr;
    int width_=0,height_=0;
    Theme* current_theme_=nullptr;
    lv_obj_t *network_label_=nullptr,*status_label_=nullptr,*notification_label_=nullptr;
    lv_obj_t *mute_label_=nullptr,*battery_label_=nullptr,*low_battery_popup_=nullptr,*low_battery_label_=nullptr;
    virtual bool Lock(int=0)=0;
    virtual void Unlock()=0;
public:
    virtual ~LvglDisplay();
    virtual void SetStatus(const char*);
    virtual void SetEmotion(const char*) {}
    virtual void SetChatMessage(const char*,const char*) {}
    virtual void SetTheme(Theme*) {}
    virtual void ShowNotification(const char*,int=3000);
    virtual void ShowNotification(const std::string& text,int ms=3000) { ShowNotification(text.c_str(),ms); }
};
