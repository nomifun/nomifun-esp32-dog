#pragma once
#include "audio_codec.h"
class Board {
    AudioCodec codec_;
public:
    static Board& GetInstance() { static Board value; return value; }
    AudioCodec* GetAudioCodec() { return &codec_; }
};
