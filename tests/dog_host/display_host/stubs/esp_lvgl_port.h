#pragma once
#include <lvgl.h>
#include <cstdint>
struct lvgl_port_cfg_t { int task_priority=0; int task_stack=0; int task_affinity=0; };
#define ESP_LVGL_PORT_INIT_CONFIG() {}
struct lvgl_port_display_cfg_t {
    void* io_handle; void* panel_handle; void* control_handle; uint32_t buffer_size;
    bool double_buffer; uint32_t trans_size; uint32_t hres; uint32_t vres; bool monochrome;
    struct { bool swap_xy; bool mirror_x; bool mirror_y; } rotation;
    struct { unsigned buff_dma:1; unsigned buff_spiram:1; unsigned sw_rotate:1; unsigned full_refresh:1; unsigned direct_mode:1; } flags;
};
inline int lvgl_port_init(const lvgl_port_cfg_t*) { return 0; }
lv_display_t* lvgl_port_add_disp(const lvgl_port_display_cfg_t* cfg);
inline int lvgl_port_deinit() { return 0; }
inline bool lvgl_port_lock(int) { return true; }
inline void lvgl_port_unlock() {}
