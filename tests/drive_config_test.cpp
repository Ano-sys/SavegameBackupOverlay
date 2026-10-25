#include "include/GoogleDriveConfig.hpp"
#include "include/JsonHandler.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
bool emulate_switch_rename = true;
bool fail_install = false;

std::string read(const std::string& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

extern "C" int __real_rename(const char* source, const char* destination);
extern "C" int __wrap_rename(const char* source, const char* destination) {
    const std::string from(source), to(destination);

    if(emulate_switch_rename && std::filesystem::exists(to)) {
        errno = EEXIST;
        return -1;
    }

    if(fail_install && from.ends_with(".tmp") && std::filesystem::exists(to + ".bak")) {
        errno = EIO;
        return -1;
    }

    return __real_rename(source, destination);
}

int main() {
    std::string error;
    drive::DeviceAuthorization device;
    const std::string authorization = R"({"device_code":"private-device-code","user_code":"GQVQ-JKEC",
        "verification_url":"https://www.google.com/device","expires_in":1800,"interval":5})";
    assert(drive::parse_device_authorization(authorization, device, error));
    assert(device.user_code == "GQVQ-JKEC");
    assert(device.qr_url() == "https://www.google.com/device");
    assert(device.verification_uri_complete.empty());
    assert(device.interval == 5 && device.expires_in == 1800);

    const std::string complete = R"({"device_code":"private","user_code":"012345",
        "verification_uri":"https://example.test/activate","expires_in":300,
        "verification_uri_complete":"https://example.test/activate?user_code=012345"})";
    assert(drive::parse_device_authorization(complete, device, error));
    assert(device.qr_url() == "https://example.test/activate?user_code=012345");
    assert(device.user_code == "012345" && device.interval == 5);
    assert(!drive::parse_device_authorization(
        R"({"device_code":"a","user_code":"x",
        "verification_url":"http://example.test","expires_in":300})",
        device, error
    ));
    assert(!drive::parse_device_authorization(
        R"({"device_code":"a","user_code":"x",
        "verification_url":"https://example.test","expires_in":300,"interval":0})",
        device, error
    ));
    assert(!drive::parse_device_authorization(authorization + " trailing-data", device, error));
    assert(!drive::parse_device_authorization("{", device, error));
    assert(drive::oauth_error(R"({"error":"authorization_pending"})") == "authorization_pending");
    assert(drive::form_encode("a+b /&=\n") == "a%2Bb%20%2F%26%3D%0A");

    drive::Tokens token;
    assert(
        drive::parse_tokens(
            R"({"access_token":"access","refresh_token":"refresh",
        "token_type":"Bearer","expires_in":3600,
        "scope":"https://www.googleapis.com/auth/drive.file"})",
            token, error
        )
    );
    assert(!drive::parse_tokens(
        R"({"access_token":"a","token_type":"Bearer","expires_in":3600,
        "scope":"https://www.googleapis.com/auth/drive.file"})",
        token, error
    ));
    assert(!drive::parse_tokens(
        R"({"access_token":"a","refresh_token":"r","token_type":"Bearer",
        "expires_in":3600,"scope":"https://www.googleapis.com/auth/drive.file.invalid"})",
        token, error
    ));
    const drive::Tokens previous_token{"old-access", "keep-refresh", drive::Scope, 1};
    assert(drive::parse_refreshed_tokens(R"({"access_token":"new-access","token_type":"Bearer","expires_in":3600})", previous_token, token, error));
    assert(token.access_token == "new-access" && token.refresh_token == "keep-refresh" && token.scope == drive::Scope);
    assert(!drive::parse_refreshed_tokens(R"({"access_token":"a","token_type":"Bearer","expires_in":3600,"scope":"other"})", previous_token, token, error));

    char directory[] = "/tmp/sbo-drive-test-XXXXXX";
    assert(::mkdtemp(directory));
    const std::string path = std::string(directory) + "/nested/auth.json";
    drive::GoogleDriveConfig config(path);
    drive::Credentials credentials;
#if TEST_BUNDLED_OAUTH
    assert(config.credentials(credentials, error));
    assert(credentials.client_id == "test-build-client.apps.googleusercontent.com");
    assert(credentials.client_secret == "test-build-secret");
#else
    assert(!config.credentials(credentials, error));
    assert(error.find("unavailable in this build") != std::string::npos);
#endif
    // A new installation needs no manually prepared auth.json or OAuth template.
    assert(!std::filesystem::exists(path));

    JsonHandler json(path);
    assert(json.load(true));
    assert(!cJSON_HasObjectItem(json.root(), "oauth"));
    assert(JsonHandler::set_string(json.root(), "unrelated", "preserve-me"));
    assert(json.save());

    // Existing empty templates also use the bundled app.
    cJSON* oauth = JsonHandler::object(json.root(), "oauth", true);
    assert(JsonHandler::set_string(oauth, "client_id", ""));
    assert(JsonHandler::set_string(oauth, "client_secret", ""));
    assert(json.save());
#if TEST_BUNDLED_OAUTH
    assert(config.credentials(credentials, error));
    assert(credentials.client_id == "test-build-client.apps.googleusercontent.com");
    assert(credentials.client_secret == "test-build-secret");
#else
    assert(!config.credentials(credentials, error));
#endif

    // Never combine half of a custom OAuth client with the bundled client.
    assert(JsonHandler::set_string(oauth, "client_id", "test-client"));
    assert(json.save());
    assert(!config.credentials(credentials, error));
    assert(credentials.client_id.empty() && credentials.client_secret.empty());
    assert(JsonHandler::set_string(oauth, "client_secret", "test-secret"));
    assert(json.save());
    assert(config.credentials(credentials, error));
    assert(credentials.client_id == "test-client" && credentials.client_secret == "test-secret");

    const auto legacy_configuration = read(path);
    cJSON_DeleteItemFromObjectCaseSensitive(json.root(), "oauth");
    assert(cJSON_AddBoolToObject(json.root(), "oauth", true));
    assert(json.save());
    assert(!config.credentials(credentials, error));
    cJSON_DeleteItemFromObjectCaseSensitive(json.root(), "oauth");
    oauth = JsonHandler::object(json.root(), "oauth", true);
    assert(cJSON_AddNumberToObject(oauth, "client_id", 123));
    assert(json.save());
    assert(!config.credentials(credentials, error));
    {
        std::ofstream file(path);
        file << legacy_configuration;
    }

    const drive::Tokens global{"global-access", "global-refresh", drive::Scope, 1900000000};
    const drive::Tokens own{"own-access", "own-refresh", drive::Scope, 1900000000};
    std::optional<drive::Tokens> selected;
    std::string resolved;
    assert(config.effective_connection("user:a", selected, resolved, error));
    assert(!selected && resolved.empty());
    assert(config.save_connection("global", global, error));
    assert(config.effective_connection("user:a", selected, resolved, error));
    assert(selected && selected->refresh_token == "global-refresh" && resolved == "global");
    assert(config.save_connection("user:a", own, error));
    assert(config.effective_connection("user:a", selected, resolved, error));
    assert(selected && selected->refresh_token == "own-refresh" && resolved == "user:a");
    assert(config.effective_connection("user:b", selected, resolved, error));
    assert(selected && selected->refresh_token == "global-refresh" && resolved == "global");

    assert(json.load());
    auto* connections = JsonHandler::object(json.root(), "connections");
    auto* entry = JsonHandler::object(connections, "user:a");
    assert(JsonHandler::set_string(entry, "folder_id", "preserve-folder"));
    assert(JsonHandler::set_string(entry, "refresh_token", ""));
    assert(json.save());
    assert(!config.effective_connection("user:a", selected, resolved, error));
    assert(!selected && resolved.empty());
    assert(config.save_connection("user:a", own, error));
    assert(json.load());
    assert(JsonHandler::string(JsonHandler::object(JsonHandler::object(json.root(), "connections"), "user:a"), "folder_id") == "preserve-folder");
    assert(config.remove_connection("user:a", error));
    assert(config.effective_connection("user:a", selected, resolved, error));
    assert(selected && selected->refresh_token == "global-refresh" && resolved == "global");
    assert(json.load());
    assert(JsonHandler::string(json.root(), "unrelated") == "preserve-me");
    assert(JsonHandler::string(JsonHandler::object(json.root(), "oauth"), "client_secret") == "test-secret");

    assert(config.save_connection("user:a", own, error));
    assert(config.remove_connection("global", error));
    assert(config.effective_connection("user:a", selected, resolved, error));
    assert(selected && selected->refresh_token == "own-refresh" && resolved == "user:a");
    assert(config.effective_connection("user:b", selected, resolved, error));
    assert(!selected && resolved.empty());

    // Failed writes and malformed files must never replace existing credentials.
    const std::string previous = read(path);
    fail_install = true;
    assert(!config.save_connection("user:c", own, error));
    assert(read(path) == previous);
    assert(!std::filesystem::exists(path + ".bak"));
    fail_install = false;

    // Simulate a power loss between moving the old file and installing the new one.
    assert(std::rename(path.c_str(), (path + ".bak").c_str()) == 0);
    assert(config.credentials(credentials, error));
    assert(credentials.client_id == "test-client");
    assert(read(path) == previous);
    assert(!std::filesystem::exists(path + ".bak"));

    emulate_switch_rename = false;
    assert(config.save_connection("user:c", own, error));
    assert(config.remove_connection("user:c", error));
    emulate_switch_rename = true;
    const std::string before_failure = read(path);
    std::filesystem::create_directory(path + ".tmp");
    assert(!config.save_connection("user:c", own, error));
    assert(read(path) == before_failure);
    std::filesystem::remove(path + ".tmp");
    {
        std::ofstream file(path);
        file << "{broken json";
    }

    assert(!config.save_connection("user:c", own, error));
    assert(!config.remove_connection("global", error));
    assert(!config.credentials(credentials, error));
    assert(read(path) == "{broken json");

    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    assert(!cleanup_error);
    std::puts("OAuth parsing, JsonHandler persistence, and global/user selection passed.");
}
