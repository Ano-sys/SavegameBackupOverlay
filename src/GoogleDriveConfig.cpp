#include "include/GoogleDriveConfig.hpp"
#include "include/JsonHandler.hpp"
#include "GoogleOAuthCredentials.hpp"

#include <cmath>
#include <ctime>
#include <memory>
#include <mutex>
#include <utility>

namespace drive {
namespace {
std::mutex storage_mutex;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

Json parse(const std::string& text) {
    if(text.size() > 64 * 1024 || text.find('\0') != std::string::npos) return Json(nullptr, cJSON_Delete);

    return Json(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true), cJSON_Delete);
}

int positive_integer(const cJSON* parent, const char* key, int fallback = 0) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(parent, key);

    if(!value) return fallback;
    if(!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 1 || value->valuedouble > 86400 || std::floor(value->valuedouble) != value->valuedouble) return 0;

    return static_cast<int>(value->valuedouble);
}

bool https_url(const std::string& value) { return value.size() <= 2048 && value.starts_with("https://") && value.size() > 8 && value.find_first_of("\r\n\t ") == std::string::npos; }

bool load_connection(JsonHandler& json, const std::string& key, std::optional<Tokens>& out, std::string& error) {
    out.reset();

    cJSON* connections = JsonHandler::object(json.root(), "connections");

    if(!connections) {
        if(cJSON_HasObjectItem(json.root(), "connections")) {
            error = "'connections' in auth.json must be an object.";
            return false;
        }

        return true;
    }

    cJSON* entry = cJSON_GetObjectItemCaseSensitive(connections, key.c_str());

    if(!entry) return true;
    if(!cJSON_IsObject(entry)) {
        error = "Stored Drive connection is invalid. Connect again.";
        return false;
    }

    Tokens tokens;
    tokens.access_token = JsonHandler::string(entry, "access_token");
    tokens.refresh_token = JsonHandler::string(entry, "refresh_token");
    tokens.scope = JsonHandler::string(entry, "scope");

    const cJSON* expires = cJSON_GetObjectItemCaseSensitive(entry, "expires_at");

    if(cJSON_IsNumber(expires) && std::isfinite(expires->valuedouble) && expires->valuedouble >= 0 && expires->valuedouble <= 253402300799.0) tokens.expires_at = static_cast<int64_t>(expires->valuedouble);

    if(tokens.refresh_token.empty()) {
        error = "Refresh token is missing. Sign in to this connection again.";
        return false;
    }

    out = std::move(tokens);
    return true;
}
} // namespace

std::string oauth_error(const std::string& response) {
    const auto json = parse(response);
    return JsonHandler::string(json.get(), "error");
}

bool parse_device_authorization(const std::string& response, DeviceAuthorization& out, std::string& error) {
    out = {};
    error.clear();

    const auto json = parse(response);

    if(!cJSON_IsObject(json.get())) {
        error = "Invalid response from Google.";
        return false;
    }

    out.device_code = JsonHandler::string(json.get(), "device_code");
    out.user_code = JsonHandler::string(json.get(), "user_code");
    out.verification_url = JsonHandler::string(json.get(), "verification_url");

    if(out.verification_url.empty()) out.verification_url = JsonHandler::string(json.get(), "verification_uri");

    out.verification_uri_complete = JsonHandler::string(json.get(), "verification_uri_complete");

    if(!out.verification_uri_complete.empty() && !https_url(out.verification_uri_complete)) {
        error = "Invalid Google sign-in URL.";
        return false;
    }

    out.expires_in = positive_integer(json.get(), "expires_in");
    out.interval = positive_integer(json.get(), "interval", 5);

    if(out.device_code.empty() || out.device_code.size() > 4096 || out.user_code.empty() || out.user_code.size() > 128 || !https_url(out.verification_url) || out.expires_in == 0 || out.interval == 0) {
        error = "Google did not return a valid sign-in code.";
        return false;
    }

    return true;
}

bool parse_tokens(const std::string& response, Tokens& out, std::string& error) {
    out = {};
    error.clear();

    const auto json = parse(response);
    out.access_token = JsonHandler::string(json.get(), "access_token");
    out.refresh_token = JsonHandler::string(json.get(), "refresh_token");
    out.scope = JsonHandler::string(json.get(), "scope");
    const int lifetime = positive_integer(json.get(), "expires_in");

    if(out.access_token.empty() || out.refresh_token.empty() || lifetime == 0 || JsonHandler::string(json.get(), "token_type") != "Bearer") {
        error = "Google did not return complete sign-in credentials.";
        return false;
    }

    // Device authorization must grant file access before this connection can be used.
    bool file_access = false;

    for(size_t begin = 0; begin < out.scope.size();) {
        const size_t end = out.scope.find(' ', begin);

        if(out.scope.substr(begin, end == std::string::npos ? end : end - begin) == Scope) file_access = true;

        if(end == std::string::npos) break;

        begin = end + 1;
    }

    if(!file_access) {
        error = "Google Drive file access was not granted.";
        return false;
    }

    out.expires_at = static_cast<int64_t>(std::time(nullptr)) + lifetime;
    return true;
}

std::string form_encode(const std::string& value) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result;

    for(unsigned char c : value) {
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            result += static_cast<char>(c);
        }
        else {
            result += '%';
            result += digits[c >> 4];
            result += digits[c & 15];
        }
    }

    return result;
}

bool parse_refreshed_tokens(const std::string& response, const Tokens& previous, Tokens& out, std::string& error) {
    const auto json = parse(response);

    if(!cJSON_IsObject(json.get())) {
        error = "Invalid token refresh response from Google.";
        return false;
    }

    if(!cJSON_HasObjectItem(json.get(), "refresh_token")) JsonHandler::set_string(json.get(), "refresh_token", previous.refresh_token);

    if(!cJSON_HasObjectItem(json.get(), "scope")) JsonHandler::set_string(json.get(), "scope", previous.scope);

    char* text = cJSON_PrintUnformatted(json.get());

    if(!text) {
        error = "Could not read refreshed credentials.";
        return false;
    }

    const bool ok = parse_tokens(text, out, error);
    cJSON_free(text);
    return ok;
}

GoogleDriveConfig::GoogleDriveConfig(std::string path) : path(std::move(path)) {}

bool GoogleDriveConfig::credentials(Credentials& out, std::string& error) {
    std::lock_guard lock(storage_mutex);

    error.clear();
    out = {};
    JsonHandler json(path);

    if(!json.load(true)) {
        error = json.error();
        return false;
    }

    const cJSON* oauth = cJSON_GetObjectItemCaseSensitive(json.root(), "oauth");

    if(oauth && !cJSON_IsObject(oauth)) {
        error = "The optional 'oauth' override in auth.json must be an object.";
        return false;
    }

    for(const char* field : {"client_id", "client_secret"}) {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(oauth, field);

        if(value && !cJSON_IsString(value)) {
            error = "The custom OAuth app credentials must be strings.";
            return false;
        }
    }

    out.client_id = JsonHandler::string(oauth, "client_id");
    out.client_secret = JsonHandler::string(oauth, "client_secret");

    // Preserve existing connections created with a custom OAuth app. Fresh
    // installs and old empty templates use the app bundled with the release.
    if(out.client_id.empty() && out.client_secret.empty()) out = {build::ClientId, build::ClientSecret};

    if(out.client_id.empty() || out.client_secret.empty()) {
        error = out.client_id.empty() && out.client_secret.empty() ? "Google sign-in is unavailable in this build. Install a configured release."
                                                                 : "The custom OAuth app configuration is incomplete.";
        out = {};
        return false;
    }

    return true;
}

bool GoogleDriveConfig::connection(const std::string& key, std::optional<Tokens>& out, std::string& error) {
    std::lock_guard lock(storage_mutex);

    error.clear();
    out.reset();
    JsonHandler json(path);

    if(!json.load(true)) {
        error = json.error();
        return false;
    }

    return load_connection(json, key, out, error);
}

bool GoogleDriveConfig::effective_connection(const std::string& key, std::optional<Tokens>& out, std::string& resolved_key, std::string& error) {
    std::lock_guard lock(storage_mutex);

    error.clear();
    out.reset();
    resolved_key.clear();
    JsonHandler json(path);

    if(!json.load(true)) {
        error = json.error();
        return false;
    }

    if(!load_connection(json, key, out, error)) return false;
    if(out) {
        resolved_key = key;
        return true;
    }

    if(key != "global") {
        if(!load_connection(json, "global", out, error)) return false;
        if(out) resolved_key = "global";
    }

    return true;
}

bool GoogleDriveConfig::save_connection(const std::string& key, const Tokens& tokens, std::string& error) {
    std::lock_guard lock(storage_mutex);

    error.clear();
    JsonHandler json(path);

    if(!json.load(true)) {
        error = json.error();
        return false;
    }

    cJSON* connections = JsonHandler::object(json.root(), "connections", true);
    cJSON* entry = JsonHandler::object(connections, key.c_str(), true);

    // An explicitly configured but invalid entry can be repaired by connecting again.

    if(connections && !entry) {
        cJSON_DeleteItemFromObjectCaseSensitive(connections, key.c_str());
        entry = JsonHandler::object(connections, key.c_str(), true);
    }

    if(tokens.refresh_token.empty() || !entry || !JsonHandler::set_string(entry, "access_token", tokens.access_token) || !JsonHandler::set_string(entry, "refresh_token", tokens.refresh_token) || !JsonHandler::set_string(entry, "scope", tokens.scope)) {
        error = "Could not save Drive connection.";
        return false;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(entry, "expires_at");

    if(!cJSON_AddNumberToObject(entry, "expires_at", static_cast<double>(tokens.expires_at)) || !json.save()) {
        error = json.error().empty() ? "Could not save Drive connection." : json.error();
        return false;
    }

    return true;
}

bool GoogleDriveConfig::remove_connection(const std::string& key, std::string& error) {
    std::lock_guard lock(storage_mutex);

    error.clear();
    JsonHandler json(path);

    if(!json.load(true)) {
        error = json.error();
        return false;
    }

    cJSON* connections = JsonHandler::object(json.root(), "connections");

    if(!connections) {
        if(cJSON_HasObjectItem(json.root(), "connections")) {
            error = "'connections' in auth.json must be an object.";
            return false;
        }

        return true;
    }

    if(!cJSON_HasObjectItem(connections, key.c_str())) return true;

    cJSON_DeleteItemFromObjectCaseSensitive(connections, key.c_str());

    if(!json.save()) {
        error = json.error();
        return false;
    }

    return true;
}

bool GoogleDriveConfig::folder_id(const std::string& key, std::string& out, std::string& error) {
    std::lock_guard lock(storage_mutex);
    out.clear();
    error.clear();
    JsonHandler json(path);

    if(!json.load()) {
        error = json.error();
        return false;
    }

    const auto* entry = cJSON_GetObjectItemCaseSensitive(JsonHandler::object(json.root(), "connections"), key.c_str());
    const auto* folder = cJSON_GetObjectItemCaseSensitive(entry, "folder_id");

    if(folder && !cJSON_IsString(folder)) {
        error = "Drive folder_id must be a string.";
        return false;
    }

    out = JsonHandler::string(entry, "folder_id");
    return true;
}

} // namespace drive
