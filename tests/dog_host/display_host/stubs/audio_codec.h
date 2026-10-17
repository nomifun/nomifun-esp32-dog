#pragma once
#include <algorithm>
class AudioCodec {
    int volume_ = 50;
public:
    int set_volume_calls = 0;
    int output_volume() const { return volume_; }
    void SetOutputVolume(int volume) { ++set_volume_calls; volume_ = std::clamp(volume, 0, 100); }
};
