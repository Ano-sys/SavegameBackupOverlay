#pragma once

#include "BackupTypes.hpp"
#include "GoogleDriveConfig.hpp"

#include <curl/curl.h>
#include <chrono>
#include <functional>
#include <vector>

namespace drive {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    std::string upload_path;
    std::string download_path;
    uint64_t transfer_size = 0;
    std::string operation{};
    long timeout_seconds = 0;
    long connect_timeout_seconds = 10;
    uint64_t response_limit = 2 * 1024 * 1024;
};

struct HttpReply {
    bool transport_ok = false;
    long status = 0;
    std::string body;
    std::string location;
    std::string error{};
    bool success() const { return transport_ok && status >= 200 && status < 300; }
};

using HttpTransport = std::function<HttpReply(const HttpRequest&, const backup::Control&)>;

class CurlSession {
public:
    CurlSession() = default;
    ~CurlSession() = default;
    CurlSession(const CurlSession&) = delete;
    CurlSession& operator=(const CurlSession&) = delete;
    HttpReply request(const HttpRequest& request, const backup::Control& control) const;
    static constexpr auto IdleTimeout = std::chrono::minutes(5);
    // Nonblocking maintenance; active requests are never interrupted.
    static bool expire_idle(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Call once all overlay workers have stopped.
    static void shutdown();
};

class DriveStorage {
public:
    DriveStorage(GoogleDriveConfig& config, HttpTransport transport, backup::Control control = {});
    bool connect(const std::string& connection_key, std::string& error);
    bool list(const backup::Identity& identity, std::vector<backup::CloudBackup>& out, std::string& error);
    bool list_game(const backup::Identity& identity, std::vector<backup::CloudBackup>& out, std::string& error);
    bool upload(const backup::Identity& identity, const std::string& path, const std::string& save_hash, backup::CloudBackup& out, std::string& error);
    bool download(const backup::CloudBackup& entry, const std::string& path, std::string& error);
    bool trash_backup(const backup::Identity& identity, const backup::CloudBackup& entry, std::string& error);

private:
    bool refresh(std::string& error);
    bool request(HttpRequest request, HttpReply& out, std::string& error);
    bool folder(const std::string& parent, const std::string& name, bool create, std::string& out, std::string& error, const std::string& property = {}, const std::string& value = {}, const backup::Identity* source = nullptr);
    bool save_folder(const backup::Identity& identity, bool create, std::string& out, std::string& error);
    bool list_folder(const backup::Identity& identity, const std::string& parent, std::vector<backup::CloudBackup>& out, std::string& error);

    GoogleDriveConfig& config;
    HttpTransport transport;
    backup::Control control;
    Tokens tokens;
    std::string key;
    std::string root_id;
};

} // namespace drive
