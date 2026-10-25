#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace drive {

inline constexpr const char* AuthFile = "sdmc:/switch/.overlays/SwitchCloudBackup/auth.json";
inline constexpr const char* Scope = "https://www.googleapis.com/auth/drive.file";

struct Credentials {
    std::string client_id;
    std::string client_secret;
};

struct Tokens {
    std::string access_token;
    std::string refresh_token;
    std::string scope;
    int64_t expires_at = 0;
};

struct DeviceAuthorization {
    std::string device_code;
    std::string user_code;
    std::string verification_url;
    std::string verification_uri_complete;
    int expires_in = 0;
    int interval = 5;

    const std::string& qr_url() const { return verification_uri_complete.empty() ? verification_url : verification_uri_complete; }
};

bool parse_device_authorization(const std::string& response, DeviceAuthorization& out, std::string& error);
bool parse_tokens(const std::string& response, Tokens& out, std::string& error);
bool parse_refreshed_tokens(const std::string& response, const Tokens& previous, Tokens& out, std::string& error);
std::string oauth_error(const std::string& response);
std::string form_encode(const std::string& value);

class GoogleDriveConfig {
public:
    explicit GoogleDriveConfig(std::string path = AuthFile);

    bool credentials(Credentials& out, std::string& error);
    bool connection(const std::string& key, std::optional<Tokens>& out, std::string& error);
    bool effective_connection(const std::string& key, std::optional<Tokens>& out, std::string& resolved_key, std::string& error);
    bool save_connection(const std::string& key, const Tokens& tokens, std::string& error);
    bool remove_connection(const std::string& key, std::string& error);
    bool folder_id(const std::string& key, std::string& out, std::string& error);

private:
    std::string path;
};

} // namespace drive
