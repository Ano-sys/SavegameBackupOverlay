#pragma once

#include "BackupTypes.hpp"
#include "GoogleDriveConfig.hpp"

#include <curl/curl.h>
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
    CurlSession();
    ~CurlSession();
    CurlSession(const CurlSession&) = delete;
    CurlSession& operator=(const CurlSession&) = delete;
    bool ready() const { return initialized; }
    const std::string& error() const { return initialization_error; }
    HttpReply request(const HttpRequest& request, const backup::Control& control) const;

private:
    bool sockets = false;
    bool initialized = false;
    CURL* handle = nullptr;
    std::string initialization_error;
};

class DriveStorage {
public:
    DriveStorage(GoogleDriveConfig& config, HttpTransport transport, backup::Control control = {});
    bool connect(const std::string& connection_key, std::string& error);
    bool list(const backup::Identity& identity, std::vector<backup::CloudBackup>& out, std::string& error);
    bool upload(const backup::Identity& identity, const std::string& path, const std::string& save_hash, backup::CloudBackup& out, std::string& error);
    bool download(const backup::CloudBackup& entry, const std::string& path, std::string& error);
    bool trash_backup(const backup::Identity& identity, const backup::CloudBackup& entry, std::string& error);

private:
    bool refresh(std::string& error);
    bool request(HttpRequest request, HttpReply& out, std::string& error);
    bool folder(const std::string& parent, const std::string& name, bool create, std::string& out, std::string& error, const std::string& property = {}, const std::string& value = {});
    bool save_folder(const backup::Identity& identity, bool create, std::string& out, std::string& error);

    GoogleDriveConfig& config;
    HttpTransport transport;
    backup::Control control;
    Tokens tokens;
    std::string key;
    std::string root_id;
};

} // namespace drive
