#pragma once
#include "lvgl_font.h"
#include <memory>
#include <map>
#include <string>
#include <vector>
class Theme { public: virtual ~Theme() = default; };
class LvglTheme : public Theme {
    std::shared_ptr<LvglFont> text_{std::make_shared<LvglBuiltInFont>(&font_puhui_14_1)};
    std::shared_ptr<LvglFont> icons_{std::make_shared<LvglBuiltInFont>(&font_awesome_14_1)};
    std::shared_ptr<LvglFont> large_{std::make_shared<LvglBuiltInFont>(&font_awesome_30_1)};
public:
    explicit LvglTheme(const std::string& = "host") {}
    std::shared_ptr<LvglFont> text_font() const { return text_; }
    std::shared_ptr<LvglFont> icon_font() const { return icons_; }
    std::shared_ptr<LvglFont> large_icon_font() const { return large_; }
    void set_text_font(std::shared_ptr<LvglFont> font) { text_=std::move(font); }
    void set_icon_font(std::shared_ptr<LvglFont> font) { icons_=std::move(font); }
    void set_large_icon_font(std::shared_ptr<LvglFont> font) { large_=std::move(font); }
};
class LvglThemeManager {
    std::vector<std::unique_ptr<LvglTheme>> themes_;
public:
    static LvglThemeManager& GetInstance() { static LvglThemeManager value; return value; }
    void RegisterTheme(const std::string&,LvglTheme* theme) { themes_.emplace_back(theme); }
};
