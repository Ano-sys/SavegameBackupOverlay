#include "include/Diagnostics.hpp"
#include "include/BackupTypes.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <cstdint>
#include <malloc.h>
extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;
#endif

namespace backup {
std::string filesystem_error(const char* operation, std::string_view path, int code, std::optional<uint32_t> last_fs_result) {
    const auto detail = code ? std::strerror(code) : "no errno reported";
    std::string message = std::string(operation) + " '" + std::string(path) + "': " + detail + " (errno " + std::to_string(code) + ")";
    if(last_fs_result) {
        const auto result = *last_fs_result;
        char native[96]{};
        std::snprintf(native, sizeof(native), " [last FS Result 0x%08X, module %u, description %u]", static_cast<unsigned>(result), static_cast<unsigned>(result & 0x1ff), static_cast<unsigned>((result >> 9) & 0x1fff));
        message += native;
    }
    return message + ".";
}

void log_memory(const char* stage) {
#ifdef __SWITCH__
    const auto info = mallinfo();
    const auto begin = reinterpret_cast<uintptr_t>(fake_heap_start);
    const auto end = reinterpret_cast<uintptr_t>(fake_heap_end);
    const auto capacity = end >= begin ? end - begin : 0;
    char message[320]{};
    std::snprintf(message, sizeof(message), "Memory %s: heap=%zu used=%zu arena=%zu free_chunks=%zu top=%zu chunks=%zu", stage, static_cast<size_t>(capacity), info.uordblks, info.arena, info.fordblks, info.keepcost, info.ordblks);
    log_error(message);
#else
    (void)stage;
#endif
}

std::string diagnostic_text(std::string text, std::initializer_list<std::string_view> secrets) {
    for(const auto secret : secrets) {
        if(secret.empty()) continue;

        for(size_t pos = 0; (pos = text.find(secret, pos)) != std::string::npos; pos += 10) text.replace(pos, secret.size(), "[redacted]");
    }

    // Upload-session URLs may themselves contain credentials. Keep their entire
    // value out of diagnostics, including URL paths and query parameters.
    for(const auto scheme : {"https://", "http://"}) {
        for(size_t pos = 0; (pos = text.find(scheme, pos)) != std::string::npos;) {
            const auto end = text.find_first_of(" \t\r\n\"'<>)]}", pos);
            text.replace(pos, end == std::string::npos ? text.size() - pos : end - pos, "[url omitted]");
            pos += 13;
        }
    }

    for(size_t pos = 0; (pos = text.find("Bearer ", pos)) != std::string::npos;) {
        const auto begin = pos + 7;
        const auto end = text.find_first_of(" \t\r\n\"'", begin);
        text.replace(begin, end == std::string::npos ? text.size() - begin : end - begin, "[redacted]");
        pos = begin + 10;
    }

    for(char& c : text) {
        if(static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
    }

    if(text.size() > 2048) {
        size_t end = 2048;
        while(end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        text.resize(end);
        text += "...";
    }

    return text;
}

bool log_error(const std::string& message) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const auto text = diagnostic_text(message);
    std::fprintf(stderr, "[SwitchCloudBackup] %s\n", text.c_str());

    const auto path = std::string(DataDirectory) + "/error.log";
    struct stat info{};
    const bool truncate = stat(path.c_str(), &info) == 0 && info.st_size >= 64 * 1024;
    FILE* file = std::fopen(path.c_str(), truncate ? "w" : "a");
    if(!file) return false;

    ::chmod(path.c_str(), 0600);
    const auto now = std::time(nullptr);
    std::tm utc{};
    char timestamp[32] = "unknown time";
    if(gmtime_r(&now, &utc)) std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    const bool written = std::fprintf(file, "[%s] %s\n", timestamp, text.c_str()) >= 0;
    return std::fclose(file) == 0 && written;
}
} // namespace backup
