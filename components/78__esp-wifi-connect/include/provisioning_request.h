#ifndef PROVISIONING_REQUEST_H_
#define PROVISIONING_REQUEST_H_

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <cJSON.h>

using ProvisioningJson = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

// NVS strings cannot represent decoded NUL. Require one complete JSON object,
// including when an HTTP request arrived in several receive calls.
inline ProvisioningJson ParseProvisioningJson(const char* body, size_t length) {
    const auto invalid = []() { return ProvisioningJson(nullptr, cJSON_Delete); };
    if (!body || length == 0 || length > 1024 || std::memchr(body, '\0', length)) return invalid();
    bool in_string = false;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char value = static_cast<unsigned char>(body[index]);
        if (in_string) {
            if (value < 0x20) return invalid();
            if (value == '\\') {
                if (++index >= length) return invalid();
                if (body[index] == 'u' && index + 4 < length &&
                    std::memcmp(body + index + 1, "0000", 4) == 0) return invalid();
            } else if (value == '"') {
                in_string = false;
            }
        } else if (value == '"') {
            in_string = true;
        } else if (value < 0x20 && value != '\t' && value != '\r' && value != '\n') {
            return invalid();
        }
    }
    const std::string text(body, length);
    ProvisioningJson root(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(root.get())) return invalid();
    for (const cJSON* item = root->child; item; item = item->next) {
        if (!item->string) return invalid();
        for (const cJSON* previous = root->child; previous != item; previous = previous->next) {
            if (std::strcmp(previous->string, item->string) == 0) return invalid();
        }
    }
    return root;
}

#endif
