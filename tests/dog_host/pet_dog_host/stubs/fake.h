
#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
using TickType_t=uint32_t; using BaseType_t=int; using gpio_num_t=int; using ledc_channel_t=int;
constexpr int pdPASS=1,pdTRUE=1; constexpr int configTICK_RATE_HZ=1000;
#define pdMS_TO_TICKS(x) (uint32_t(x))
#define portTICK_PERIOD_MS 1
constexpr int LEDC_TIMER_1=1,LEDC_LOW_SPEED_MODE=0,LEDC_TIMER_13_BIT=13,LEDC_AUTO_CLK=0,LEDC_INTR_DISABLE=0;
constexpr int LEDC_CHANNEL_0=0,LEDC_CHANNEL_1=1,LEDC_CHANNEL_2=2,LEDC_CHANNEL_3=3;
struct ledc_timer_config_t { int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg; };
struct ledc_channel_config_t { int gpio_num,speed_mode,channel,intr_type,timer_sel,duty,hpoint; };
using esp_err_t=int; constexpr int ESP_OK=0,ESP_FAIL=-1;
inline const char* esp_err_to_name(int) { return "fake error"; }
#define ESP_ERROR_CHECK(x) assert((x)==0)
inline std::mutex log_mutex;
inline std::vector<std::string> info_logs;
inline void fake_log(const char*, const char* format, ...) {
 char line[512]; va_list args; va_start(args,format);vsnprintf(line,sizeof(line),format,args);va_end(args);
 std::lock_guard lock(log_mutex);info_logs.emplace_back(line);
}
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
inline std::atomic<int> writes{0},created_tasks{0},live_tasks{0};
inline std::atomic<bool> pause_action_worker{false},action_worker_waiting{false};
inline std::array<std::atomic<uint32_t>,4> duty{};
inline int ledc_timer_config(void*){return 0;} inline int ledc_channel_config(void*){return 0;}
inline void ledc_stop(int,int,int){}
inline void ledc_set_duty(int,int channel,uint32_t value){duty[channel]=value; ++writes;}
inline void ledc_update_duty(int,int){}
struct FakeTask {std::mutex mutex; std::condition_variable cv; int notifications=0;};
using TaskHandle_t=FakeTask*;
inline thread_local TaskHandle_t current_task=nullptr;
inline int xTaskCreate(void (*entry)(void*),const char*name,int,void*arg,int,TaskHandle_t*out){
 auto task=new FakeTask;*out=task; ++created_tasks; ++live_tasks;
 const bool action_worker=strcmp(name,"ActWkr")==0;
 std::thread([=]{current_task=task;
  if(action_worker){action_worker_waiting=true;while(pause_action_worker.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));action_worker_waiting=false;}
  entry(arg); --live_tasks;}).detach();return pdPASS;
}
inline void vTaskDelay(uint32_t ticks){std::this_thread::sleep_for(std::chrono::milliseconds(ticks));}
inline void vTaskDelete(void*){}
inline void xTaskNotifyGive(TaskHandle_t task){std::lock_guard lock(task->mutex);++task->notifications; task->cv.notify_one();}
inline uint32_t ulTaskNotifyTake(int,uint32_t ticks){
 std::unique_lock lock(current_task->mutex);current_task->cv.wait_for(lock,std::chrono::milliseconds(ticks),[]{return current_task->notifications>0;});
 auto n=current_task->notifications;current_task->notifications=0;return n;
}
inline uint32_t xTaskGetTickCount(){static auto start=std::chrono::steady_clock::now();return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();}
using nvs_handle_t=uint32_t; constexpr int NVS_READONLY=0,NVS_READWRITE=1;
inline std::mutex nvs_mutex; inline std::map<std::string,std::string> nvs_values;
inline std::map<uint32_t,std::string> nvs_names;inline std::map<uint32_t,std::map<std::string,std::string>> nvs_pending;
inline uint32_t next_nvs_handle=1;inline bool fail_commit=false;
inline int nvs_open(const char*ns,int mode,nvs_handle_t*out){std::lock_guard lock(nvs_mutex);*out=next_nvs_handle++;nvs_names[*out]=ns;return 0;}
inline void nvs_close(nvs_handle_t h){std::lock_guard lock(nvs_mutex);nvs_names.erase(h);nvs_pending.erase(h);}
inline int nvs_set_str(nvs_handle_t h,const char*key,const char*value){std::lock_guard lock(nvs_mutex);nvs_pending[h][nvs_names[h]+"/"+key]=value;return 0;}
inline int nvs_commit(nvs_handle_t h){std::lock_guard lock(nvs_mutex);if(fail_commit)return -1;for(auto&[key,value]:nvs_pending[h])nvs_values[key]=value;return 0;}
inline int nvs_get_str(nvs_handle_t h,const char*key,char*value,size_t*length){
 std::lock_guard lock(nvs_mutex);auto it=nvs_values.find(nvs_names[h]+"/"+key);if(it==nvs_values.end())return -1;
 if(value)memcpy(value,it->second.c_str(),it->second.size()+1);*length=it->second.size()+1;return 0;
}
enum DeviceState{kDeviceStateUnknown,kDeviceStateIdle,kDeviceStateListening,kDeviceStateSpeaking};
struct Application {static Application&GetInstance(){static Application a;return a;}DeviceState GetDeviceState(){return kDeviceStateIdle;}};
struct Display{void SetEmotion(const char*){}};
struct Board{static Board&GetInstance(){static Board b;return b;}Display*GetDisplay(){return nullptr;}};
