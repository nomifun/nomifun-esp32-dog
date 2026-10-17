#pragma once
#include <lvgl.h>
extern "C" {
LV_FONT_DECLARE(font_puhui_14_1);
LV_FONT_DECLARE(font_awesome_14_1);
LV_FONT_DECLARE(font_awesome_30_1);
}
class LvglFont {
public:
    virtual ~LvglFont()=default;
    virtual const lv_font_t* font() const=0;
};
class LvglBuiltInFont : public LvglFont {
    const lv_font_t* font_;
public:
    explicit LvglBuiltInFont(const lv_font_t* font): font_(font) {}
    const lv_font_t* font() const { return font_; }
};
class LvglCBinFont : public LvglFont {
public:
    // Explicit host substitute: never interprets an ESP32 32-bit cbin pointer.
    explicit LvglCBinFont(void*) {}
    const lv_font_t* font() const override { return &font_puhui_14_1; }
};
