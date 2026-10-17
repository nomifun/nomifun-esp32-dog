// Compiles the actual board display source against actual bundled LVGL.
// Actual OledDisplay layout is compiled; ESP/LvglDisplay lifecycle, fonts,
// media/assets and NVS remain explicit substitutes. Images are SOFTWARE PREVIEW.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#define private public
#define protected public
#include "display_board.h"
#undef private
#undef protected
#include "application.h"
#include "board.h"
#include "settings.h"
#include "esp_lvgl_port.h"
#include "src/widgets/label/lv_label_private.h"
bool HostValidGif(const std::vector<uint8_t>& bytes);
void HostPortTransform(lv_display_t*,const lv_area_t*,uint8_t*);

static std::array<uint8_t,128*64> pixels, logical_pixels;
static std::array<uint8_t,128*64/8> page_bits;
alignas(4) static std::array<uint16_t,128*64> draw_buffer;
alignas(4) static std::array<uint16_t,128*64> converted_buffer;
static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* data) {
    // Exact production default: full RGB565 buffer, then blue > 16 and
    // inverted vertical-page bits from _lvgl_port_transform_monochrome.
    const auto* rgb565=reinterpret_cast<const uint16_t*>(data);
    for (int y=area->y1; y<=area->y2; ++y) for (int x=area->x1; x<=area->x2; ++x) {
        if(x<0||x>=128||y<0||y>=64) continue;
        const uint16_t color=rgb565[y*128+x];
        const unsigned r=((color>>11)&31)*255/31,g=((color>>5)&63)*255/63,b=(color&31)*255/31;
        logical_pixels[y*128+x]=uint8_t((77*r+150*g+29*b)>>8);
    }
    std::memcpy(converted_buffer.data(),data,sizeof(converted_buffer));
    // This executes an unchanged function extracted from the actual bundled
    // esp_lvgl_port source, not an independent approximation of its threshold.
    HostPortTransform(display,area,reinterpret_cast<uint8_t*>(converted_buffer.data()));
    std::memcpy(page_bits.data(),converted_buffer.data(),page_bits.size());
    for(int y=0;y<64;++y) for(int x=0;x<128;++x)
        pixels[y*128+x]=(page_bits[128*(y>>3)+x]&(1<<(y%8)))?255:0;
    lv_display_flush_ready(display);
}
lv_display_t* lvgl_port_add_disp(const lvgl_port_display_cfg_t* cfg) {
    if(cfg->hres!=128||cfg->vres!=64||!cfg->monochrome||cfg->buffer_size!=8192) throw std::runtime_error("Unexpected production OLED display config");
    auto* display=lv_display_create(128,64);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer.data(),nullptr,sizeof(draw_buffer),LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display,flush);
    return display;
}
LvglDisplay::~LvglDisplay() { if(low_battery_popup_) lv_obj_delete(low_battery_popup_); lv_display_delete(display_); }
void LvglDisplay::SetStatus(const char* status) {
    lv_label_set_text(status_label_,status); lv_obj_remove_flag(status_label_,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(notification_label_,LV_OBJ_FLAG_HIDDEN);
}
void LvglDisplay::ShowNotification(const char* value,int) {
    lv_label_set_text(notification_label_,value); lv_obj_remove_flag(notification_label_,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(status_label_,LV_OBJ_FLAG_HIDDEN);
}

static void advance(int ms) {
    for(int elapsed=0; elapsed<ms; elapsed+=20) { lv_tick_inc(20); lv_timer_handler(); }
    lv_refr_now(nullptr);
}
static std::string escape(const char* text) {
    std::string out;
    for(const unsigned char* s=(const unsigned char*)text; *s; ++s) {
        if(*s=='"'||*s=='\\') { out+='\\'; out+=char(*s); }
        else if(*s=='\n') out+="\\n"; else if(*s=='\r') out+="\\r";
        else if(*s>=32) out+=char(*s);
    }
    return out;
}
struct Capture {
    std::string output; bool strict=false; int captures=0, clipping=0;
    std::ofstream geometry;
    std::ofstream scrolling;
    std::ofstream selector_visual;
    int selector_checks=0;
    explicit Capture(std::string path,bool check): output(std::move(path)),strict(check),geometry(output+"/geometry.jsonl"),scrolling(output+"/scrolling.jsonl"),selector_visual(output+"/selector-visual.jsonl") {
        selector_visual << "{\"scope\":\"User clarification: the frame marks the primary choice; arrows share its text line; the separate caption is explanatory. This replaces the previous shared-column name/state centering assumption.\",\"primary_ink_roi\":[44,25,113,42],\"caption_ink_roi\":[4,46,123,63]}\n";
    }
    void Save(Sh1106Display& display,const std::string& name) {
        advance(40);
        std::ofstream image(output+"/"+name+".pgm",std::ios::binary);
        image << "P5\n128 64\n255\n"; image.write(reinterpret_cast<char*>(pixels.data()),pixels.size());
        std::ofstream logical(output+"/"+name+"_logical.pgm",std::ios::binary);
        logical << "P5\n128 64\n255\n"; logical.write(reinterpret_cast<char*>(logical_pixels.data()),logical_pixels.size());
        std::ofstream pages(output+"/"+name+"_oled_pagebits.bin",std::ios::binary);
        pages.write(reinterpret_cast<char*>(page_bits.data()),page_bits.size());
        std::ofstream rgb(output+"/"+name+"_logical_rgb565.bin",std::ios::binary);
        rgb.write(reinterpret_cast<char*>(draw_buffer.data()),sizeof(draw_buffer));
        ++captures;
        geometry << "{\"capture\":\"" << name << "\",\"menu_active\":" << (display.MenuActive()?"true":"false") << ",\"labels\":[";
        bool first=true;
        std::vector<lv_obj_t*> labels={display.menu_title_,display.menu_value_,display.menu_info_};
#ifdef DISPLAY_HOST_CAROUSEL
        labels.insert(labels.end(),{display.menu_counter_,display.menu_left_,display.menu_right_});
        if(display.MenuActive()) {
            const std::array<lv_obj_t*,7> objects={display.menu_title_,display.menu_counter_,display.menu_icon_,display.menu_value_,display.menu_info_,display.menu_left_,display.menu_right_};
            const int xs[]={4,84,14,43,4,0,119},ys[]={0,0,22,22,46,25,25},ws[]={76,40,24,72,120,9,9},hs[]={18,18,24,24,18,18,18};
            for(size_t i=0;i<objects.size();++i) {
                lv_area_t bounds; lv_obj_get_coords(objects[i],&bounds);
                if(strict&&(bounds.x1!=xs[i]||bounds.y1!=ys[i]||lv_area_get_width(&bounds)!=ws[i]||lv_area_get_height(&bounds)!=hs[i]))
                    throw std::runtime_error("Carousel object geometry differs from its reserved slot");
            }
            if(strict&&reinterpret_cast<lv_label_t*>(display.menu_counter_)->text_size.x>40)
                throw std::runtime_error("Carousel counter "+std::string(lv_label_get_text(display.menu_counter_))+" width="+std::to_string(reinterpret_cast<lv_label_t*>(display.menu_counter_)->text_size.x)+" exceeds slot 40");
            int icon_lit=0; for(int y=22;y<46;++y) for(int x=14;x<38;++x) if(pixels[y*128+x]) ++icon_lit;
            if(strict&&icon_lit==0) throw std::runtime_error("Carousel icon must render visible pixels");
        }
#endif
        for(auto* label : labels) {
            if(!first) geometry << ','; first=false;
            lv_area_t area; lv_obj_get_coords(label,&area);
            lv_area_t visible=area;
            for(auto* ancestor=lv_obj_get_parent(label); ancestor; ancestor=lv_obj_get_parent(ancestor)) {
                lv_area_t bounds; lv_obj_get_coords(ancestor,&bounds);
                visible.x1=std::max(visible.x1,bounds.x1); visible.y1=std::max(visible.y1,bounds.y1);
                visible.x2=std::min(visible.x2,bounds.x2); visible.y2=std::min(visible.y2,bounds.y2);
            }
            bool cut=area.x1<0||area.y1<0||area.x2>127||area.y2>63||visible.x1!=area.x1||visible.x2!=area.x2||visible.y1!=area.y1||visible.y2!=area.y2;
            if(cut&&display.MenuActive()) ++clipping;
            geometry << "{\"text\":\"" << escape(lv_label_get_text(label)) << "\",\"x\":" << area.x1 << ",\"y\":" << area.y1
                     << ",\"w\":" << lv_area_get_width(&area) << ",\"h\":" << lv_area_get_height(&area) << ",\"clipped\":" << (cut?"true":"false") << '}';
        }
        geometry << "],\"header_labels\":[";
        first=true;
        for(auto* label : {display.network_label_,display.mute_label_,display.battery_label_,display.status_label_,display.notification_label_}) {
            if(!first) geometry << ','; first=false;
            lv_area_t area; lv_obj_get_coords(label,&area);
            geometry << "{\"text\":\"" << escape(lv_label_get_text(label)) << "\",\"x\":" << area.x1 << ",\"y\":" << area.y1
                     << ",\"w\":" << lv_area_get_width(&area) << ",\"h\":" << lv_area_get_height(&area) << '}';
        }
        geometry << "]}\n";
#ifdef DISPLAY_HOST_CAROUSEL
        if(display.MenuActive()) SelectorPixels(display,name);
#endif
    }
    void Scroll(lv_obj_t* label,const char* name) {
        const auto* value=reinterpret_cast<lv_label_t*>(label);
        scrolling << "{\"sample\":\"" << name << "\",\"offset_x\":" << value->offset.x
                  << ",\"offset_y\":" << value->offset.y << ",\"text_width\":" << value->text_size.x << ",\"text_height\":" << value->text_size.y << "}\n";
    }
    struct Ink { int left=128,right=-1,top=64,bottom=-1,count=0; };
    static Ink InkIn(int x1,int y1,int x2,int y2) {
        Ink value;
        for(int y=y1;y<=y2;++y) for(int x=x1;x<=x2;++x) if(pixels[y*128+x]) {
            value.left=std::min(value.left,x);value.right=std::max(value.right,x);
            value.top=std::min(value.top,y);value.bottom=std::max(value.bottom,y);++value.count;
        }
        return value;
    }
    void SelectorPixels(Sh1106Display& display,const std::string& scene) {
        // Border pixels are deliberately excluded: a centered rectangle must
        // not conceal off-center or missing text inside the primary choice.
        const Ink choice=InkIn(44,25,113,42),caption=InkIn(4,46,123,63);
        const Ink left=InkIn(0,25,8,42),right=InkIn(119,25,127,42);
        const bool short_choice=reinterpret_cast<lv_label_t*>(display.menu_value_)->text_size.x<=70;
        const bool short_caption=reinterpret_cast<lv_label_t*>(display.menu_info_)->text_size.x<=120;
        auto centered=[](const Ink& value,int target2) { return value.count>0&&value.left+value.right>=target2-4&&value.left+value.right<=target2+4; };
        int border=0;
        for(int x=43;x<=114;++x) { if(pixels[22*128+x]) ++border;if(pixels[45*128+x]) ++border; }
        for(int y=23;y<=44;++y) { if(pixels[y*128+43]) ++border;if(pixels[y*128+114]) ++border; }
        lv_area_t content,explanation,left_bounds,right_bounds;
        lv_obj_get_content_coords(display.menu_value_,&content);lv_obj_get_coords(display.menu_info_,&explanation);
        lv_obj_get_coords(display.menu_left_,&left_bounds);lv_obj_get_coords(display.menu_right_,&right_bounds);
        const bool same_text_row=content.y1==25&&content.y2==42&&left_bounds.y1==25&&left_bounds.y2==42&&right_bounds.y1==25&&right_bounds.y2==42;
        const bool separate_caption=explanation.y1==46&&content.y2<explanation.y1;
        selector_visual << "{\"scene\":\"" << scene << "\",\"selection\":" << display.selection_
                        << ",\"frame_border_pixels\":" << border << ",\"arrows_and_primary_same_text_row\":" << (same_text_row?"true":"false")
                        << ",\"caption_below_primary\":" << (separate_caption?"true":"false")
                        << ",\"primary\":{\"text\":\"" << escape(lv_label_get_text(display.menu_value_)) << "\",\"short_text\":" << (short_choice?"true":"false")
                        << ",\"ink_left\":" << choice.left << ",\"ink_right\":" << choice.right << ",\"ink_center_x\":" << (choice.left+choice.right)/2.0
                        << ",\"ink_center_y\":" << (choice.top+choice.bottom)/2.0 << ",\"target_center_x\":78.5},\"caption\":{\"text\":\"" << escape(lv_label_get_text(display.menu_info_))
                        << "\",\"short_text\":" << (short_caption?"true":"false") << ",\"ink_left\":" << caption.left << ",\"ink_right\":" << caption.right
                        << ",\"ink_center_x\":" << (caption.left+caption.right)/2.0 << ",\"target_center_x\":63.5},\"arrow_ink_centers_y\":[" << (left.top+left.bottom)/2.0 << ',' << (right.top+right.bottom)/2.0 << "]}\n";
        ++selector_checks;
        if(!strict) return;
        if(border!=188) throw std::runtime_error("Primary choice must have a complete visible 1px frame: "+scene);
        if(!same_text_row||!separate_caption) throw std::runtime_error("Primary choice, arrows and separate caption violate their rows: "+scene);
        if(!short_choice) throw std::runtime_error("Fixed primary option must fit its 70px content width without scrolling: "+scene);
        if(!choice.count||!left.count||!right.count) throw std::runtime_error("Primary choice and arrows must all render visible ink: "+scene);
        if(short_choice&&!centered(choice,157)) throw std::runtime_error("Primary text ink is not centered inside the frame: "+scene);
        if(short_caption&&!centered(caption,127)) throw std::runtime_error("Explanatory caption ink is not centered across the screen: "+scene);
        if(short_choice&&((choice.top+choice.bottom)-(left.top+left.bottom)>4||(left.top+left.bottom)-(choice.top+choice.bottom)>4))
            throw std::runtime_error("Arrow ink and primary text ink are not on the same visual row: "+scene);
        if(short_choice&&((choice.top+choice.bottom)-(right.top+right.bottom)>4||(right.top+right.bottom)-(choice.top+choice.bottom)>4))
            throw std::runtime_error("Right arrow ink and primary text ink are not on the same visual row: "+scene);
    }
};
static void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static std::vector<uint8_t> hex(const std::string& text) {
    std::vector<uint8_t> bytes;
    for(size_t i=0;i<text.size();i+=2) bytes.push_back(uint8_t(std::stoul(text.substr(i,2),nullptr,16)));
    return bytes;
}
static void VerifyVisibleNavigation(Sh1106Display& display,Capture& capture,
                                    std::vector<std::string>& actions,std::vector<std::string>& setting_callbacks,
                                    std::vector<std::string>& info_requests,bool& alarm_enabled) {
    using Page=Sh1106Display::Page;
    auto* codec=Board::GetInstance().GetAudioCodec();
    std::ofstream audit(capture.output+"/navigation.jsonl");
    struct Snapshot {
        size_t actions,settings;int writes,volume_calls,volume;bool alarm;
        std::map<std::string,bool> nvs;std::string emotion;
    };
    auto snapshot=[&]() { return Snapshot{actions.size(),setting_callbacks.size(),Settings::WriteCount(),codec->set_volume_calls,codec->output_volume(),alarm_enabled,Settings::Snapshot(),display.emotion_}; };
    auto unchanged=[&](const Snapshot& value) {
        return value.actions==actions.size()&&value.settings==setting_callbacks.size()&&value.writes==Settings::WriteCount()&&
               value.volume_calls==codec->set_volume_calls&&value.volume==codec->output_volume()&&value.alarm==alarm_enabled&&
               value.nvs==Settings::Snapshot()&&value.emotion==display.emotion_;
    };
    auto open_apps=[&]() {
        display.ExitMenu();check(!display.MenuActive(),"Navigation path must start in Chat");
        display.MenuConfirm();check(display.MenuActive()&&display.page_==Page::Apps,"Center from Chat must open Apps");
    };
    auto seek=[&](const char* name) {
        const int count=display.PageItemCountLocked();
        for(int i=0;i<count;++i) {
            if(std::string(lv_label_get_text(display.menu_value_))==name) return;
            display.MenuNext();
        }
        throw std::runtime_error("Visible menu option is missing: "+std::string(name));
    };
    auto open_settings=[&]() { open_apps();seek("设置");display.MenuConfirm();check(display.page_==Page::Settings,"Visible settings app must open Settings"); };
    advance(3500);display.Tick(); // Expire earlier unrelated fixture notices.
    struct Route { const char* app;const char* scene;Page page; };
    const Route routes[]={{"表情","emotion",Page::Emotion},{"音乐","music",Page::Music},{"闹钟","alarm",Page::Alarm},{"设置","settings",Page::Settings},{"关于","about",Page::About}};
    for(const auto& route:routes) for(int direction:{1,-1}) {
        open_apps();seek(route.app);const int parent_selection=display.selection_;
        const Snapshot initial=snapshot();display.MenuConfirm();
        check(display.page_==route.page,"Visible app entry must reach its expected page");
        const int count=display.PageItemCountLocked();check(count>=2,"Child page must include a visible return item");
        for(int i=0;i<count&&display.selection_!=0;++i) display.MenuPrevious();
        check(display.selection_==0,"Child traversal must start at its first content item");
        const std::string prefix="nav_"+std::string(route.scene)+(direction>0?"_next":"_previous");
        capture.Save(display,prefix+"_first");
        int last_real=0;
        const int steps=direction>0?count-1:count+1;
        for(int i=0;i<steps;++i) {
            const size_t before_info=info_requests.size();
            if(direction>0) display.MenuNext();else display.MenuPrevious();
            check(display.selection_>=0&&display.selection_<count,"Every menu selection must remain inside the production item count");
            if(display.IsBackItemLocked()) {
                check(display.InfoKeyLocked().empty(),"Visible return item must not index service information arrays");
                check(info_requests.size()==before_info,"Moving onto return must not query a media/about information field");
            } else last_real=display.selection_;
        }
        check(display.IsBackItemLocked()&&display.selection_==count-1,"Both traversal directions must reach the final visible return item");
        check(std::string(lv_label_get_text(display.menu_value_))=="返回上级","Child return label must be visibly explicit");
        capture.Save(display,prefix+"_return");
        const size_t before_confirm_info=info_requests.size();display.MenuConfirm();
        check(display.page_==Page::Apps&&display.selection_==parent_selection&&display.MenuActive(),"Visible return confirmation must restore the selected parent app");
        check(unchanged(initial),"Child traversal and visible return must not invoke actions, write settings/volume, change emotion/alarm, or reset network");
        check(info_requests.size()==before_confirm_info,"Confirming return must not query another service field");
        capture.Save(display,prefix+"_parent");
        display.MenuConfirm();check(display.page_==route.page&&display.selection_==last_real&&!display.IsBackItemLocked(),"Reopening a child must restore its most recent real content item, not the return entry");
        capture.Save(display,prefix+"_reentered");
        display.MenuBack();check(display.page_==Page::Apps&&display.selection_==parent_selection,"Long-back compatibility must preserve the parent selection too");
        check(unchanged(initial),"Reentry and long-back must also be side-effect free");
        audit << "{\"path\":\"" << prefix << "\",\"items\":" << count << ",\"return_index\":" << count-1 << ",\"parent_selection\":" << parent_selection << ",\"reentered_content_selection\":" << last_real << ",\"side_effects\":false,\"return_info_queries\":0}\n";
    }
    open_apps();
    const Snapshot before_exit=snapshot();int last_app=display.selection_;
    for(int i=0,count=display.PageItemCountLocked();i<count&&!display.IsBackItemLocked();++i) {last_app=display.selection_;display.MenuNext();}
    check(display.IsBackItemLocked()&&std::string(lv_label_get_text(display.menu_value_))=="退出菜单","Apps must expose a visible Exit menu entry");
    capture.Save(display,"nav_apps_exit_selected");display.MenuConfirm();
    check(display.page_==Page::Chat&&!display.MenuActive()&&unchanged(before_exit),"Apps Exit must return to Chat without chat, music, alarm or reset callbacks");
    capture.Save(display,"nav_apps_exit_chat");display.MenuNext();
    check(display.page_==Page::Apps&&display.selection_==last_app&&!display.IsBackItemLocked(),"Apps must reopen at its last real app, not Exit");
    audit << "{\"path\":\"apps_exit\",\"side_effects\":false,\"chat_action_emitted\":false,\"reopened_real_app\":" << last_app << "}\n";

    open_settings();seek("音量设置");const int volume_setting=display.selection_;const Snapshot before_volume=snapshot();
    display.MenuConfirm();check(display.page_==Page::Volume,"Volume setting must open the numeric volume page");
    check(std::string(lv_label_get_text(display.menu_info_)).find("中键返回")!=std::string::npos,"Volume page must visibly explain center-button return");
    capture.Save(display,"nav_volume_center_return_prompt");display.MenuConfirm();
    check(display.page_==Page::Settings&&display.selection_==volume_setting&&unchanged(before_volume),"Center from Volume must return to its selected setting without writing volume or settings");
    capture.Save(display,"nav_volume_returned_settings");
    audit << "{\"path\":\"volume_center_return\",\"setting_selection\":" << volume_setting << ",\"side_effects\":false}\n";

    open_settings();seek("重新配网");const int reset_setting=display.selection_;const Snapshot before_reset=snapshot();
    check(reset_setting==display.PageItemCountLocked()-2,"Network reset must remain before the newly appended Settings return item");
    display.MenuConfirm();check(display.page_==Page::ResetNetworkConfirm&&display.PageItemCountLocked()==2&&display.selection_==0,"Real Settings route must open reset confirmation at Cancel");
    capture.Save(display,"nav_reset_route_cancel");display.MenuPrevious();
    check(display.selection_==1&&std::string(lv_label_get_text(display.menu_value_))=="清除重启","Reset left wrap must still select destructive option index 1");
    capture.Save(display,"nav_reset_route_destructive_selection");display.MenuNext();check(display.selection_==0,"Reset right wrap must still return to Cancel index 0");
    display.MenuConfirm();check(display.page_==Page::Settings&&display.selection_==reset_setting&&unchanged(before_reset),"Reset Cancel must preserve its Settings position without emitting reset or writes");
    display.MenuConfirm();check(display.page_==Page::ResetNetworkConfirm&&display.selection_==0,"Every real-route reset visit must default to Cancel");
    display.MenuBack();display.MenuNext();check(display.IsBackItemLocked(),"Settings return must be distinct from its reset option");
    capture.Save(display,"nav_settings_return_after_reset");display.MenuConfirm();
    check(display.page_==Page::Apps&&unchanged(before_reset),"Settings visible return after reset navigation must not erase the network");
    audit << "{\"path\":\"reset_and_settings_return\",\"reset_setting_index\":" << reset_setting << ",\"cancel_index\":0,\"destructive_index\":1,\"side_effects\":false}\n";

    open_apps();seek("闹钟");display.MenuConfirm();
    for(int index=0;index<2;++index) {
        seek(index==0?"时间":"歌曲");const Snapshot before=snapshot();display.MenuConfirm();
        check(unchanged(before)&&display.page_==Page::Alarm,"Alarm time/song confirmation must be read-only and must not toggle enabled");
        check(display.notification_text_=="仅供查看","Read-only alarm fields must explain their confirmation behavior");
        capture.Save(display,index==0?"nav_alarm_time_read_only":"nav_alarm_song_read_only");
    }
    advance(3500);display.Tick();seek("闹钟开关");
    const size_t before_toggle=actions.size();const bool initial_enabled=alarm_enabled;display.MenuConfirm();
    check(actions.size()==before_toggle+1&&actions.back()=="alarm_toggle"&&alarm_enabled!=initial_enabled,"Only the explicit alarm switch item must dispatch one toggle");
    capture.Save(display,"nav_alarm_explicit_switch");
    audit << "{\"path\":\"alarm_confirmation\",\"read_only_fields\":2,\"read_only_side_effects\":false,\"explicit_switch_callbacks\":1}\n";

    open_apps();seek("关于");const int about_app=display.selection_;display.MenuConfirm();
    while(display.IsBackItemLocked()) display.MenuNext();const Snapshot before_about=snapshot();display.MenuConfirm();
    check(display.page_==Page::Apps&&display.selection_==about_app&&unchanged(before_about),"About content confirmation must retain the previous return-to-parent compatibility");
    capture.Save(display,"nav_about_content_confirm_parent");
    audit << "{\"path\":\"about_content_confirm\",\"side_effects\":false,\"parent_selection\":" << about_app << "}\n";
    std::cout << "PASS: visible return paths from Chat through five apps in both directions, parent/content memory, callback/NVS/codec/emotion/alarm side-effect audit, Apps Exit, Volume center return, reset indices and Alarm read-only fields\n";
}
int main(int argc,char** argv) {
    if(argc<3) return 2;
    try {
        lv_init(); Capture capture(argv[1],std::string(argv[2])=="after");
        const std::string gif="47494638396101000100800000000000ffffff2c0000000001000100000202440100";
        check(HostValidGif(hex(gif+"3b")),"Valid one-frame GIF must remain accepted");
        check(HostValidGif(hex(gif+"21ff0b4e45545343415045322e3003010000003b")),"Valid Netscape loop extension must remain accepted");
        if(capture.strict) {
            check(!HostValidGif(hex(gif+"21ff0b4e45545343415045322e30003b")),"Truncated Netscape loop extension must be rejected before gifdec");
            check(!HostValidGif(hex(gif+"21ff0b4e45545343415045616263003b")),"Truncated Netscape extension with alternate auth must be rejected");
        }
        LvglTheme alternate_theme;
        Sh1106Display display(nullptr,nullptr,128,64,false,false);
        std::vector<std::string> actions;
        std::vector<std::string> settings;
        std::vector<std::string> info_requests;
        bool alarm_enabled=true;
        std::string music="正在播放 12/105\nThe longest music title for OLED scrolling acceptance.mp3";
        std::string alarm="已开启\n07:30\n每天重复";
        std::string about="机器人小狗\nWiFi: A very long network name\nIP: 192.168.123.100";
        display.SetMenuCallbacks([&](const std::string& action){actions.push_back(action);if(action=="alarm_toggle") alarm_enabled=!alarm_enabled;},[&](const std::string& key){
            info_requests.push_back(key);
            if(key=="music") return music;
            if(key=="alarm") return alarm;
            if(key=="alarm_time") return std::string("07:30 每天");
            if(key=="alarm_music") return std::string("The longest alarm song name for OLED acceptance.mp3");
            if(key=="alarm_state") return std::string(alarm_enabled?"开启":"关闭");
            if(key=="about_version") return std::string("v2.0.0-menu-host");
            if(key=="about_network") return std::string("192.168.123.100");
            if(key=="about_board") return std::string("zfsn-esp32s3-dog");
            if(key=="about") return about;
            throw std::runtime_error("Unexpected/out-of-range menu information key: "+key);
        });
        display.OnSettingChanged([&](const std::string& key,bool){settings.push_back(key);});
        capture.Save(display,"chat");
        display.MenuNext(); check(display.MenuActive(),"Next must open Apps");
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"apps_"+std::to_string(i)); display.MenuNext(); }
        display.MenuBack(); check(!display.MenuActive(),"Back from Apps must return chat");
        display.ShowPageLocked(Sh1106Display::Page::Emotion);
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"emotion_"+std::to_string(i)); display.MenuNext(); }
        display.ShowPageLocked(Sh1106Display::Page::Music); display.Tick();
        if(capture.strict) check(std::string(lv_label_get_text(display.menu_info_)).find('\n')==std::string::npos,"Menu service content must use a single scrollable line");
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"music_"+std::to_string(i)); display.MenuNext(); }
        capture.Save(display,"music_scroll_0");
        for(int i=1;i<=8;++i) { advance(1000); display.Tick(); capture.Save(display,"music_scroll_"+std::to_string(i)); }
        // Record actual LVGL label offsets before and immediately after service
        // refresh. This verifies the current renderer instead of assuming how
        // the library treats repeated lv_label_set_text calls.
        music="A single line extremely long filename that must keep scrolling across service refresh updates.mp3";
        display.Tick();
        if(capture.strict) check(reinterpret_cast<lv_label_t*>(display.menu_info_)->offset.x==0,"Changed song content must start at its first character");
        capture.Scroll(display.menu_info_,"new_content_start");
        advance(1100); capture.Scroll(display.menu_info_,"before_tick");
        const int offset_before=reinterpret_cast<lv_label_t*>(display.menu_info_)->offset.x;
        display.Tick(); capture.Scroll(display.menu_info_,"after_tick");
        if(capture.strict) check(reinterpret_cast<lv_label_t*>(display.menu_info_)->offset.x==offset_before,"Unchanged service refresh must preserve marquee offset");
        advance(1000); capture.Scroll(display.menu_info_,"one_second_later");
        if(capture.strict) check(reinterpret_cast<lv_label_t*>(display.menu_info_)->offset.x!=offset_before,"Long text must actually scroll");
        capture.Save(display,"music_long_scroll");
        display.MenuConfirm(); check(actions.back()=="music_toggle","Music confirms must reach callback");
        display.ShowPageLocked(Sh1106Display::Page::Alarm); display.Tick();
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"alarm_"+std::to_string(i)); display.MenuNext(); }
        display.ShowPageLocked(Sh1106Display::Page::About); display.Tick();
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"about_"+std::to_string(i)); display.MenuNext(); }
        display.ShowPageLocked(Sh1106Display::Page::Settings);
        for(int i=0,count=display.PageItemCountLocked();i<count;++i) { capture.Save(display,"settings_"+std::to_string(i)); display.MenuNext(); }
#ifdef DISPLAY_HOST_CAROUSEL
        if(capture.strict) {
            // Two complete rounds in both directions: every setting is checked
            // repeatedly after genuine MenuNext/MenuPrevious transitions.
            for(int direction : {1,-1}) for(int round=0;round<2;++round) for(int step=0,count=display.PageItemCountLocked();step<count;++step) {
                const std::string scene="settings_selector_"+std::string(direction>0?"next":"previous")+"_"+std::to_string(round)+"_"+std::to_string(display.selection_);
                capture.Save(display,scene);
                if(direction>0) display.MenuNext(); else display.MenuPrevious();
            }
        }
#endif
        if(capture.strict) {
            auto* codec=Board::GetInstance().GetAudioCodec();codec->SetOutputVolume(70);
            display.ShowPageLocked(Sh1106Display::Page::Volume);capture.Save(display,"volume_selector_70");
            auto region=[](int x1,int y1,int x2,int y2) {
                std::vector<uint8_t> result;for(int y=y1;y<=y2;++y) for(int x=x1;x<=x2;++x) result.push_back(pixels[y*128+x]);return result;
            };
            const auto original_choice=region(44,25,113,42),original_caption=region(4,46,123,63);
            check(std::string(lv_label_get_text(display.menu_title_))=="音量设置"&&std::string(lv_label_get_text(display.menu_value_))=="70%"&&std::string(lv_label_get_text(display.menu_info_)).find("中键返回")!=std::string::npos,"Volume must show the numeric selected value and visible center-button return guidance");
            display.MenuNext();capture.Save(display,"volume_selector_75");
            check(codec->output_volume()==75&&std::string(lv_label_get_text(display.menu_value_))=="75%","Right must change the framed volume value");
            check(region(44,25,113,42)!=original_choice&&region(4,46,123,63)==original_caption,"Volume adjustment must change primary text pixels while preserving the explanatory caption");
            display.MenuPrevious();capture.Save(display,"volume_selector_70_returned");
            check(codec->output_volume()==70&&region(44,25,113,42)==original_choice&&region(4,46,123,63)==original_caption,"Left must restore the primary volume pixels without moving or changing the caption");
        }
        display.ShowPageLocked(Sh1106Display::Page::Volume);
        for(int i=0;i<30;++i) display.MenuNext();
        check(Board::GetInstance().GetAudioCodec()->output_volume()==100,"Volume upper clamp"); capture.Save(display,"volume_100");
        for(int i=0;i<30;++i) display.MenuPrevious();
        check(Board::GetInstance().GetAudioCodec()->output_volume()==0,"Volume lower clamp"); capture.Save(display,"volume_0");
        display.ShowPageLocked(Sh1106Display::Page::ResetNetworkConfirm);
        size_t initial=actions.size(); capture.Save(display,"reset_cancel");
        if(capture.strict) check(std::string(lv_label_get_text(display.menu_value_))=="取消"&&std::string(lv_label_get_text(display.menu_info_)).find("保留网络")!=std::string::npos,"Reset cancel must be the framed choice, with preserve-network wording only in the separate caption");
        display.MenuConfirm();
        check(actions.size()==initial,"Repeated confirm must cancel reset");
        display.ShowPageLocked(Sh1106Display::Page::ResetNetworkConfirm); display.MenuNext(); capture.Save(display,"reset_selected");
        if(capture.strict) check(std::string(lv_label_get_text(display.menu_value_))=="清除重启"&&std::string(lv_label_get_text(display.menu_info_)).find("清除网络")!=std::string::npos,"Reset destructive choice and its explanation must occupy distinct rows");
        display.MenuConfirm();
        check(actions.size()==initial+1&&actions.back()=="reset_network","Explicit reset selection must reach callback");
        display.ShowPageLocked(Sh1106Display::Page::Music); display.Tick();
        Application::state=kDeviceStateListening;
        display.SetStatus("聆听中"); display.SetChatMessage("assistant","机器人字幕更新不应中断菜单");
        capture.Save(display,"menu_during_chat_update");
        if(capture.strict) check(display.MenuActive(),"Incoming status/subtitle must preserve menu");
        Application::state=kDeviceStateIdle;
        display.ShowPageLocked(Sh1106Display::Page::Apps); display.ShowNotification("设置已保存",3000);
        capture.Save(display,"menu_notification"); advance(3500); display.Tick(); capture.Save(display,"menu_notification_expired");
        if(capture.strict) {
            check(std::string(lv_label_get_text(display.menu_info_))!="设置已保存","Notification must expire");
            display.ShowPageLocked(Sh1106Display::Page::Apps); display.selection_=4; display.MenuConfirm();
            check(display.page_==Sh1106Display::Page::Settings,"Settings route");
            display.selection_=2; display.MenuBack();
            check(display.page_==Sh1106Display::Page::Apps&&display.selection_==4,"Back must retain selected parent app");
            display.MenuConfirm(); check(display.selection_==2,"Settings must retain selected setting");
            display.MenuConfirm(); check(settings.size()==1&&settings.back()=="charging_led","Setting toggle callback must match selected setting");
            display.selection_=0; display.MenuConfirm(); check(display.VolumePageActive(),"Volume setting route");
            display.MenuBack(); check(display.page_==Sh1106Display::Page::Settings&&display.selection_==0,"Volume back must preserve setting position");
            capture.Save(display,"settings_returned_from_volume");
            display.ShowPageLocked(Sh1106Display::Page::ResetNetworkConfirm); display.MenuNext(); display.MenuBack();
            display.MenuConfirm(); check(display.page_==Sh1106Display::Page::ResetNetworkConfirm&&display.selection_==0,"Every reset visit must default to Cancel");
            capture.Save(display,"reset_revisited_defaults_cancel");
            display.ShowNotification("设置已保存\n充电灯效\r开启\t完成",3000);
            check(std::string(lv_label_get_text(display.menu_info_)).find_first_of("\r\n\t")==std::string::npos,"Menu notification must flatten control whitespace");
            capture.Save(display,"menu_multiline_notification");
#ifdef DISPLAY_HOST_NEW_UI
            display.ExitMenu(); check(!display.MenuActive(),"Explicit voice start must close menu");
            capture.Save(display,"chat_after_explicit_exit");
            lv_area_t status; lv_obj_get_coords(display.status_label_,&status);
            check(status.x1==18&&status.x2==93&&status.y1==0&&status.y2==17,"Status must fit reserved header slot without covering icons");
            const std::array<lv_obj_t*,5> header_labels={display.network_label_,display.mute_label_,display.battery_label_,display.status_label_,display.notification_label_};
            const int starts[]={0,94,110,18,18},ends[]={17,109,127,93,93};
            for(size_t i=0;i<header_labels.size();++i) {
                lv_area_t bounds; lv_obj_get_coords(header_labels[i],&bounds);
                check(bounds.x1==starts[i]&&bounds.x2==ends[i]&&bounds.y1==0&&bounds.y2==17,"Every header label must occupy its exact reserved slot");
            }
            display.MenuNext(); check(display.page_==Sh1106Display::Page::Apps&&display.selection_==4,"Menu reopening after explicit exit must remember parent app");
            capture.Save(display,"apps_reopened_after_exit");
            display.SetTheme(&alternate_theme);
            check(display.MenuActive()&&display.page_==Sh1106Display::Page::Apps&&display.selection_==4,"Theme refresh must preserve selected menu");
            capture.Save(display,"menu_after_theme_refresh");
            display.ExitMenu(); display.SetTheme(&alternate_theme);
            capture.Save(display,"chat_after_theme_refresh");
            // Exercise deletion of the header's five reparented live labels and
            // active LVGL animation while another display instance still lives.
            for(int i=0;i<10;++i) {
                Sh1106Display transient(nullptr,nullptr,128,64,false,false);
                transient.MenuNext(); transient.MenuNext(); transient.SetTheme(&alternate_theme);
                advance(40);
            }
            lv_display_set_default(display.display_);
#endif
        }
        if(capture.strict) VerifyVisibleNavigation(display,capture,actions,settings,info_requests,alarm_enabled);
        std::cout << "Captures=" << capture.captures << " clipped_menu_labels=" << capture.clipping
                  << " font_line_height=" << font_puhui_14_1.line_height << "\n";
        if(capture.strict) check(capture.clipping==0,"Menu labels must be fully contained in display and parents");
        std::cout << "PASS: actual LVGL board rendering, six apps, 21 emotions, media callbacks, volume clamps, default-cancel and explicit reset\n";
        if(capture.strict) std::cout << "PASS: menu containment, exact five header slots from real CENTER-aligned base labels, page memory, setting callback, notification normalization/expiry, status/subtitle preservation, changed/unchanged marquee offsets, explicit exit, theme refresh, ten destructor cycles and malformed Netscape GIF rejection\n";
        if(capture.strict&&capture.selector_checks) std::cout << "PASS: user-clarified framed-choice layout; arrows share the primary text row, border-excluded primary ink uses x=78.5, independent explanatory captions use x=63.5; production-count settings traversed twice in each direction\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
