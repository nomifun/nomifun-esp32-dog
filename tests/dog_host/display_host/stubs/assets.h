#pragma once
#include <string>
class Assets {
public:
    static Assets& GetInstance() { static Assets value; return value; }
    bool GetAssetData(const std::string&, void*&, size_t&) { return false; }
};
