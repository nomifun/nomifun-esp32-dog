#pragma once
#include <map>
#include <string>
class Settings {
    inline static std::map<std::string, bool> values_;
    inline static int writes_ = 0;
public:
    explicit Settings(const char*, bool = false) {}
    bool GetBool(const std::string& key, bool fallback) const { auto i=values_.find(key); return i == values_.end() ? fallback : i->second; }
    void SetBool(const std::string& key, bool value) { ++writes_; values_[key] = value; }
    static int WriteCount() { return writes_; }
    static std::map<std::string,bool> Snapshot() { return values_; }
};
