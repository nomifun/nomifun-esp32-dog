#ifndef SSID_MANAGER_H
#define SSID_MANAGER_H

#include <string>
#include <vector>

struct SsidItem {
    std::string ssid;
    std::string password;
};

class SsidManager {
public:
    static SsidManager& GetInstance() {
        static SsidManager instance;
        return instance;
    }

    bool AddSsid(const std::string& ssid, const std::string& password);
    bool RemoveSsid(int index);
    void SetDefaultSsid(int index);
    void Clear();
    const std::vector<SsidItem>& GetSsidList() const { return ssid_list_; }

private:
    SsidManager();
    ~SsidManager();

    void LoadFromNvs();
    bool SaveToNvs();

    std::vector<SsidItem> ssid_list_;
};

#endif // SSID_MANAGER_H
