#pragma once
#include <lvgl.h>
#include <functional>
class LvglGif {
public:
    explicit LvglGif(const lv_image_dsc_t*) {}
    bool IsLoaded() const { return false; }
    const lv_image_dsc_t* image_dsc() const { return nullptr; }
    void SetFrameCallback(std::function<void()>) {}
    void Start() {}
    void Resume() {}
    void Pause() {}
};
