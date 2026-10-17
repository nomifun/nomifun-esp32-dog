#pragma once
// Host-only POSIX directory adapter. Firmware continues to use FatFS dirent.
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>
struct dirent { char d_name[1024]; };
struct DIR { std::vector<std::string> names; size_t next = 0; dirent entry{}; };
inline DIR* opendir(const char* path) {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) return nullptr;
    auto directory = new DIR;
    for (std::filesystem::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
        directory->names.push_back(it->path().filename().string());
    if (error) { delete directory; return nullptr; }
    return directory;
}
inline dirent* readdir(DIR* directory) {
    if (directory->next == directory->names.size()) return nullptr;
    const auto& name = directory->names[directory->next++];
    if (name.size() >= sizeof(directory->entry.d_name)) return nullptr;
    std::memcpy(directory->entry.d_name, name.c_str(), name.size() + 1);
    return &directory->entry;
}
inline int closedir(DIR* directory) { delete directory; return 0; }
