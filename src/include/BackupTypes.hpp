#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace backup {

inline constexpr const char* DataDirectory = "sdmc:/switch/.overlays/SwitchCloudBackup";
inline constexpr uint64_t MaxArchiveBytes = (uint64_t{4} << 30) - 1;
inline constexpr uint64_t MaxSaveBytes = uint64_t{8} << 30;

struct Identity {
    std::string user_id;
    std::string application_id;
    std::string user_name{};
    std::string application_name{};
    std::string console_name{};
};

inline std::string source_name(const Identity& identity) {
    return (identity.user_name.empty() ? identity.user_id : identity.user_name) + " - " + (identity.console_name.empty() ? "Unknown Switch" : identity.console_name);
}

struct CloudBackup {
    std::string id;
    std::string name;
    std::string created_time;
    std::string save_hash;
    std::string archive_hash;
    uint64_t size = 0;
    Identity source{};
};

struct Control {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const std::string&, uint64_t, uint64_t)> progress;

    bool stopped() const { return cancel && cancel->load(); }
    void report(const std::string& message, uint64_t done = 0, uint64_t total = 0) const {
        if(progress) progress(message, done, total);
    }
};

} // namespace backup
