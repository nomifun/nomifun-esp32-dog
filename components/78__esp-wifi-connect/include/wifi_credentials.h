#ifndef WIFI_CREDENTIALS_H_
#define WIFI_CREDENTIALS_H_

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>

// ESP-IDF uses fixed-width byte arrays, not C strings, for these fields. A
// maximum-length SSID or raw PSK fills the entire array without a trailing NUL.
inline bool IsWifiCredentialsValid(const std::string& ssid, const std::string& password) {
    return !ssid.empty() && ssid.size() <= 32 && password.size() <= 64 &&
        ssid.find('\0') == std::string::npos && password.find('\0') == std::string::npos;
}

inline bool CopyWifiCredentials(const std::string& ssid, const std::string& password,
                                uint8_t (&ssid_bytes)[32], uint8_t (&password_bytes)[64]) {
    if (!IsWifiCredentialsValid(ssid, password)) return false;
    std::memset(ssid_bytes, 0, sizeof(ssid_bytes));
    std::memset(password_bytes, 0, sizeof(password_bytes));
    std::memcpy(ssid_bytes, ssid.data(), ssid.size());
    std::memcpy(password_bytes, password.data(), password.size());
    return true;
}

template <size_t Size>
inline std::string WifiSsidFromBytes(const uint8_t (&ssid_bytes)[Size]) {
    size_t length = 0;
    while (length < sizeof(ssid_bytes) && length < 32 && ssid_bytes[length] != 0) ++length;
    return std::string(reinterpret_cast<const char*>(ssid_bytes), length);
}

#endif // WIFI_CREDENTIALS_H_
