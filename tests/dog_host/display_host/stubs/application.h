#pragma once
enum DeviceState { kDeviceStateIdle, kDeviceStateStarting, kDeviceStateListening, kDeviceStateSpeaking };
class Application {
public:
    inline static DeviceState state = kDeviceStateIdle;
    static Application& GetInstance() { static Application app; return app; }
    DeviceState GetDeviceState() const { return state; }
};
