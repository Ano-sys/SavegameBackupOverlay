#include "include/OverlayUpdater.hpp"
#include "include/JsonHandler.hpp"
#include "include/SaveArchive.hpp"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace {
constexpr const char* Api = "https://api.github.com/repos/Ano-sys/SavegameBackupOverlay/releases";
constexpr const char* Downloads = "https://github.com/Ano-sys/SavegameBackupOverlay/releases/download/";
std::string payload, payload_hash;
unsigned rename_calls = 0;
unsigned fail_rename = 0;
bool fail_rollback = false;

void write(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary);
    file << bytes;
    file.close();
    assert(file.good());
}

std::string read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string encode(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    assert(text);
    std::string result(text);
    cJSON_free(text);
    cJSON_Delete(json);
    return result;
}

cJSON* release(const std::string& tag) {
    auto* json = cJSON_CreateObject();
    JsonHandler::set_string(json, "tag_name", tag);
    cJSON_AddBoolToObject(json, "draft", false);
    cJSON_AddBoolToObject(json, "prerelease", false);
    auto* assets = cJSON_AddArrayToObject(json, "assets");
    for(const std::string& name : {std::string(updater::AssetName), std::string(updater::AssetName) + ".sha256"}) {
        auto* asset = cJSON_CreateObject();
        JsonHandler::set_string(asset, "name", name);
        JsonHandler::set_string(asset, "state", "uploaded");
        JsonHandler::set_string(asset, "browser_download_url", std::string(Downloads) + tag + "/" + name);
        cJSON_AddNumberToObject(asset, "size", name.ends_with("sha256") ? 92 : payload.size());
        cJSON_AddItemToArray(assets, asset);
    }
    return json;
}

struct Server {
    std::string latest = "v2.1.0";
    std::string list;
    std::string body_override;
    std::string bytes;
    std::string checksum_body;
    std::string redirect = "https://release-assets.githubusercontent.com/github-production-release-asset/test?sig=example";
    long failure_status = 0;
    bool incomplete = false, transport_failure = false;
    unsigned calls = 0;
    std::vector<std::string> urls;
    std::atomic<bool>* cancel_after_download = nullptr;

    drive::HttpReply operator()(const drive::HttpRequest& request, const backup::Control& control) {
        ++calls;
        urls.push_back(request.url);
        for(const auto& header : request.headers) assert(!header.starts_with("Authorization:"));
        if(transport_failure) return {false, 0, "", "", "connection failed"};
        if(failure_status) return {true, failure_status, "", ""};
        if(request.url.find("?per_page=") != std::string::npos) return {true, 200, list, ""};
        if(request.url == std::string(Api) + "/latest" || request.url.starts_with(std::string(Api) + "/tags/")) {
            if(!body_override.empty()) return {true, 200, body_override, ""};
            const auto tag = request.url.ends_with("/latest") ? latest : request.url.substr(std::strlen(Api) + 6);
            return {true, 200, encode(release(tag)), ""};
        }
        if(request.url.ends_with(".sha256")) return {true, 200, checksum_body.empty() ? payload_hash + "  " + updater::AssetName + "\n" : checksum_body, ""};
        if(request.url.starts_with(Downloads)) return {true, 302, "", redirect};
        assert(request.url == redirect);
        assert(request.transfer_size == payload.size());
        write(request.download_path, incomplete ? "partial" : bytes.empty() ? payload
                                                                            : bytes);
        control.report("transfer", payload.size(), payload.size());
        if(cancel_after_download) cancel_after_download->store(true);
        return {true, 200, "", ""};
    }
};
} // namespace

extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* from, const char* to) {
    ++rename_calls;
    if(rename_calls == fail_rename || (fail_rollback && rename_calls > fail_rename)) {
        errno = EIO;
        return -1;
    }
    return __real_rename(from, to);
}

int main() {
    char directory[] = "/tmp/sbo-updater-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory), previous = fs::current_path();
    fs::current_path(root);
    fs::create_directories("sdmc:/switch/.overlays");
    const auto target = updater::overlay_path("Renamed Cloud Backup.ovl");
    assert(target == "sdmc:/switch/.overlays/Renamed Cloud Backup.ovl");
    for(const auto& argument : {target, std::string("/switch/.overlays/Renamed Cloud Backup.ovl"), std::string("switch/.overlays/Renamed Cloud Backup.ovl")}) assert(updater::overlay_path(argument) == target);
    for(const char* argument : {"", "/tmp/Other.ovl", "../Other.ovl", "sdmc:/switch/.overlays/sub/Other.ovl", "Other.nro", "Other.ovl\n"}) assert(updater::overlay_path(argument).empty());

    payload = std::string(0x200, '\0');
    payload.replace(16, 4, "NRO0");
    payload[25] = 1; // NRO image size 0x100, followed by assets and ULTR.
    payload.replace(payload.size() - 4, 4, "ULTR");
    write("payload", payload);
    std::string error, installed;
    assert(backup::hash_file("payload", payload_hash, error));

    Server server;
    std::atomic<bool> cancelled{false};
    updater::OverlayUpdater updater([&](const auto& request, const auto& control) { return server(request, control); }, {&cancelled, {}});

    write(target, "installed overlay");
    assert(updater.install("", target, installed, error));
    assert(installed == "v2.1.0" && read(target) == payload && read(target + ".previous") == "installed overlay");
    assert(!fs::exists(target + ".update"));
    assert(server.urls.front() == std::string(Api) + "/latest");

    server.urls.clear();
    assert(updater.install("v1.0.0", target, installed, error));
    assert(installed == "v1.0.0" && server.urls.front() == std::string(Api) + "/tags/v1.0.0");

    auto* versions = cJSON_CreateArray();
    cJSON_AddItemToArray(versions, release("v2.1.0"));
    auto* prerelease = release("v3.0.0");
    cJSON_ReplaceItemInObjectCaseSensitive(prerelease, "prerelease", cJSON_CreateBool(true));
    cJSON_AddItemToArray(versions, prerelease);
    auto* draft = release("v4.0.0");
    cJSON_ReplaceItemInObjectCaseSensitive(draft, "draft", cJSON_CreateBool(true));
    cJSON_AddItemToArray(versions, draft);
    auto* missing_asset = release("v1.2.3");
    cJSON_DeleteItemFromArray(cJSON_GetObjectItemCaseSensitive(missing_asset, "assets"), 1);
    cJSON_AddItemToArray(versions, missing_asset);
    cJSON_AddItemToArray(versions, release("v1.0.0"));
    server.list = encode(versions);
    std::vector<updater::Release> entries;
    bool more = true;
    assert(updater.releases(1, entries, more, error) && entries.size() == 2 && !more);
    assert(entries[0].tag == "v2.1.0" && entries[1].tag == "v1.0.0");
    versions = cJSON_CreateArray();
    for(unsigned index = 0; index < 20; ++index) cJSON_AddItemToArray(versions, release("v1.0." + std::to_string(index)));
    server.list = encode(versions);
    assert(updater.releases(2, entries, more, error) && entries.size() == 20 && more);
    assert(server.urls.back().ends_with("?per_page=20&page=2"));
    const auto calls = server.calls;
    assert(!updater.releases(0, entries, more, error) && server.calls == calls);
    assert(!updater.install("v1.0.0/../../latest", target, installed, error) && server.calls == calls);
    assert(!updater.install("", "sdmc:/wrong.ovl", installed, error) && server.calls == calls);

    const auto kept = [&] {
        assert(read(target) == "keep the installed overlay");
        assert(!fs::exists(target + ".update"));
    };
    write(target, "keep the installed overlay");
    server.incomplete = true;
    assert(!updater.install("", target, installed, error));
    kept();
    server.incomplete = false;
    server.bytes = payload;
    server.bytes[128] = 'x';
    assert(!updater.install("", target, installed, error) && error.find("SHA-256 mismatch") != std::string::npos);
    kept();
    server.bytes.clear();
    server.checksum_body = payload_hash + "  Other.ovl\n";
    assert(!updater.install("", target, installed, error));
    kept();
    server.checksum_body.clear();
    server.redirect = "http://release-assets.githubusercontent.com/unsafe";
    assert(!updater.install("", target, installed, error));
    kept();
    server.redirect = "https://github.com.attacker.example/unsafe";
    assert(!updater.install("", target, installed, error));
    kept();
    server.redirect = "https://release-assets.githubusercontent.com/test";

    server.cancel_after_download = &cancelled;
    assert(!updater.install("", target, installed, error));
    kept();
    cancelled.store(false);
    server.cancel_after_download = nullptr;
    assert(!updater.install("", target, installed, error, [] { return false; }));
    kept();

    server.failure_status = 404;
    assert(!updater.install("", target, installed, error));
    kept();
    server.failure_status = 403;
    assert(!updater.install("", target, installed, error) && error.find("rate limiting") != std::string::npos);
    kept();
    server.failure_status = 0;
    server.transport_failure = true;
    assert(!updater.install("", target, installed, error));
    kept();
    server.transport_failure = false;

    auto* wrong_release = release("v9.0.0");
    server.body_override = encode(wrong_release);
    assert(!updater.install("v1.0.0", target, installed, error));
    kept();
    server.body_override.clear();

    for(unsigned failure : {1u, 2u}) {
        rename_calls = 0;
        fail_rename = failure;
        assert(!updater.install("", target, installed, error));
        kept();
    }
    fail_rename = 2;
    rename_calls = 0;
    fail_rollback = true;
    assert(!updater.install("", target, installed, error));
    assert(!fs::exists(target) && read(target + ".previous") == "keep the installed overlay");
    assert(error.find("manually") != std::string::npos);
    fail_rename = 0;
    fail_rollback = false;
    fs::rename(target + ".previous", target);

    // Correctly hashed HTML or a malformed NRO still cannot replace the overlay.
    server.bytes = std::string(payload.size(), 'x');
    write("invalid", server.bytes);
    std::string invalid_hash;
    assert(backup::hash_file("invalid", invalid_hash, error));
    server.checksum_body = invalid_hash + "  " + updater::AssetName + "\n";
    assert(!updater.install("", target, installed, error) && error.find("valid Ultrahand overlay") != std::string::npos);
    kept();

    fs::current_path(previous);
    fs::remove_all(root);
    std::puts("Latest/specific releases, renamed overlays, pagination, SHA-256, NRO validation, redirects, cancellation, and replacement rollback passed.");
}
