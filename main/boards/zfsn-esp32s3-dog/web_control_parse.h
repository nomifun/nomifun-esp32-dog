#pragma once

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <map>
#include <string>

namespace dog_http {
inline int Hex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
inline bool Decode(const std::string& input, std::string& output) {
    output.clear();
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char ch = input[i];
        if (ch == '%') {
            if (i + 2 >= input.size()) return false;
            int hi = Hex(input[i + 1]), lo = Hex(input[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = (hi << 4) | lo;
            i += 2;
        } else if (ch == '+') {
            ch = ' ';
        }
        // Neither NUL nor control characters are valid API parameter content.
        if (ch < 0x20 || ch == 0x7f) return false;
        output.push_back(static_cast<char>(ch));
    }
    return true;
}
using Query = std::map<std::string, std::string>;
inline bool ParseQuery(const std::string& raw, Query& query) {
    query.clear();
    size_t begin = 0;
    while (begin < raw.size()) {
        const size_t end = raw.find('&', begin);
        const std::string part = raw.substr(begin, end - begin);
        const size_t equal = part.find('=');
        std::string key, value;
        if (!Decode(part.substr(0, equal), key) || key.empty() ||
            !Decode(equal == std::string::npos ? "" : part.substr(equal + 1), value) ||
            !query.emplace(key, value).second) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}
inline bool Integer(const std::string& raw, int min, int max, int& result) {
    if (raw.empty()) return false;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] < '0' || raw[i] > '9') return false;
    }
    char* end = nullptr;
    errno = 0;
    long value = strtol(raw.c_str(), &end, 10);
    if (errno == ERANGE || *end || value < min || value > max) return false;
    result = static_cast<int>(value);
    return true;
}
inline bool Enabled(const std::string& value) { return value == "1" || value == "true"; }
inline bool Rgb(const std::string& color, int& red, int& green, int& blue) {
    if (color.size() < 10 || (color.compare(0, 4, "RGB(") != 0 && color.compare(0, 4, "rgb(") != 0) ||
        color.back() != ')') return false;
    const size_t first = color.find(',', 4);
    const size_t second = first == std::string::npos ? first : color.find(',', first + 1);
    if (first == std::string::npos || second == std::string::npos) return false;
    int parsed_red, parsed_green, parsed_blue;
    if (!Integer(color.substr(4, first - 4), 0, 255, parsed_red) ||
        !Integer(color.substr(first + 1, second - first - 1), 0, 255, parsed_green) ||
        !Integer(color.substr(second + 1, color.size() - second - 2), 0, 255, parsed_blue)) return false;
    red = parsed_red; green = parsed_green; blue = parsed_blue;
    return true;
}
inline bool Filename(const std::string& name) {
    return !name.empty() && name.size() <= 128 && name != "." && name != ".." &&
        name.find_first_of("/\\:*?\"<>|\r\n") == std::string::npos;
}
// API paths are rooted at /sdcard. Reject traversal before opening any file.
inline bool SdPath(const std::string& requested, std::string& absolute) {
    std::string path = requested;
    if (path == "/sdcard") path = "/";
    else if (path.compare(0, 8, "/sdcard/") == 0) path.erase(0, 7);
    if (path.empty() || path[0] != '/') return false;
    size_t begin = 1;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const std::string part = path.substr(begin, end - begin);
        if (!Filename(part)) return false;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    absolute = "/sdcard" + path;
    return absolute.size() < 240;
}
} // namespace dog_http
