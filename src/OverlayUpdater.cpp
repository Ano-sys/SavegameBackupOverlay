#include "include/OverlayUpdater.hpp"
#include "include/JsonHandler.hpp"
#include "include/SaveArchive.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace updater {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr const char* Api = "https://api.github.com/repos/Ano-sys/SavegameBackupOverlay/releases";
constexpr const char* Downloads = "https://github.com/Ano-sys/SavegameBackupOverlay/releases/download/";
constexpr unsigned PageSize = 20;

Json parse(const std::string& body) {
    if(body.empty() || body.size() > 2 * 1024 * 1024 || body.find('\0') != std::string::npos) return Json(nullptr, cJSON_Delete);
    return Json(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true), cJSON_Delete);
}

bool valid_tag(const std::string& tag) {
    if(tag.size() < 6 || tag.size() > 16 || tag.front() != 'v') return false;
    unsigned dots = 0, digits = 0;
    bool zero = false;
    for(size_t index = 1; index < tag.size(); ++index) {
        const char c = tag[index];
        if(c == '.') {
            if(digits == 0 || ++dots > 2) return false;
            digits = 0;
        }
        else {
            if(c < '0' || c > '9' || (digits && zero)) return false;
            if(digits == 0) zero = c == '0';
            ++digits;
        }
    }
    return dots == 2 && digits > 0;
}

bool read_release(const cJSON* json, Release& out) {
    const auto* draft = cJSON_GetObjectItemCaseSensitive(json, "draft");
    const auto* prerelease = cJSON_GetObjectItemCaseSensitive(json, "prerelease");
    out = {};
    out.tag = JsonHandler::string(json, "tag_name");
    if(!cJSON_IsFalse(draft) || !cJSON_IsFalse(prerelease) || !valid_tag(out.tag)) return false;
    const auto* assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
    if(!cJSON_IsArray(assets)) return false;
    unsigned overlays = 0, checksums = 0;
    const cJSON* asset;
    cJSON_ArrayForEach(asset, assets) {
        const auto name = JsonHandler::string(asset, "name");
        if(name != AssetName && name != std::string(AssetName) + ".sha256") continue;
        const auto url = JsonHandler::string(asset, "browser_download_url");
        const auto* size = cJSON_GetObjectItemCaseSensitive(asset, "size");
        if(JsonHandler::string(asset, "state") != "uploaded" || url != std::string(Downloads) + out.tag + "/" + name || !cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || std::floor(size->valuedouble) != size->valuedouble) return false;
        if(name == AssetName) {
            if(++overlays > 1 || size->valuedouble < 0x84 || size->valuedouble > MaxOverlayBytes) return false;
            out.overlay_url = url;
            out.size = static_cast<uint64_t>(size->valuedouble);
        }
        else {
            if(++checksums > 1 || size->valuedouble < 64 || size->valuedouble > 512) return false;
            out.checksum_url = url;
        }
    }
    return overlays == 1 && checksums == 1;
}

bool allowed_url(const std::string& url) {
    if(!url.starts_with("https://") || url.find_first_of("\r\n\t #") != std::string::npos) return false;
    const auto end = url.find('/', 8);
    const auto host = url.substr(8, end == std::string::npos ? std::string::npos : end - 8);
    return host == "api.github.com" || host == "github.com" || host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com" || host == "github-releases.githubusercontent.com";
}

std::string io_error(const char* operation, int code) {
    return std::string(operation) + ": " + std::strerror(code ? code : EIO) + " (errno " + std::to_string(code) + ").";
}

bool checksum(const std::string& body, std::string& hash) {
    if(body.size() > 512 || body.find('\0') != std::string::npos) return false;
    // Streams pull C++ locale/iostream code into the overlay and consume a
    // substantial part of the loader's shared code/heap memory budget.
    const auto begin = body.find_first_not_of(" \t\r\n\v\f");
    if(begin == std::string::npos) return false;
    const auto end = body.find_first_of(" \t\r\n\v\f", begin);
    if(end == std::string::npos) return false;
    hash = body.substr(begin, end - begin);
    const auto name_begin = body.find_first_not_of(" \t\r\n\v\f", end);
    if(name_begin == std::string::npos) return false;
    const auto name_end = body.find_first_of(" \t\r\n\v\f", name_begin);
    if(name_end != std::string::npos && body.find_first_not_of(" \t\r\n\v\f", name_end) != std::string::npos) return false;
    std::string name = body.substr(name_begin, name_end == std::string::npos ? name_end : name_end - name_begin);
    if(!name.empty() && name.front() == '*') name.erase(0, 1);
    if(name != AssetName || hash.size() != 64) return false;
    for(char& c : hash) {
        if(c >= 'A' && c <= 'F') c += 'a' - 'A';
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool validate_overlay(const std::string& path, uint64_t size, std::string& error) {
    struct stat info{};
    if(::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 || static_cast<uint64_t>(info.st_size) != size) {
        error = "Downloaded overlay has an unexpected size.";
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "r+b");
    if(!file) {
        error = io_error("Could not open downloaded overlay", errno);
        return false;
    }
    std::array<unsigned char, 0x80> header{};
    char trailer[4]{};
    bool ok = std::fread(header.data(), 1, header.size(), file) == header.size();
    const uint32_t image_size = static_cast<uint32_t>(header[24]) | (static_cast<uint32_t>(header[25]) << 8) | (static_cast<uint32_t>(header[26]) << 16) | (static_cast<uint32_t>(header[27]) << 24);
    ok = ok && std::memcmp(header.data() + 16, "NRO0", 4) == 0 && image_size >= header.size() && image_size <= size - 4;
    ok = ok && std::fseek(file, -4, SEEK_END) == 0 && std::fread(trailer, 1, 4, file) == 4 && std::memcmp(trailer, "ULTR", 4) == 0;
    if(!ok) error = "The downloaded file is not a valid Ultrahand overlay.";
    if(ok && ::fsync(::fileno(file)) != 0) {
        error = io_error("Could not flush the update to SD", errno);
        ok = false;
    }
    if(std::fclose(file) != 0) {
        if(ok) error = io_error("Could not close downloaded overlay", errno);
        ok = false;
    }
    return ok;
}

struct StagedFile {
    std::string path;
    ~StagedFile() {
        if(!path.empty()) ::unlink(path.c_str());
    }
};
} // namespace

std::string overlay_path(const std::string& argument) {
    std::string name = argument;
    for(const char* prefix : {"sdmc:/switch/.overlays/", "/switch/.overlays/", "switch/.overlays/"}) {
        if(name.starts_with(prefix)) {
            name.erase(0, std::strlen(prefix));
            break;
        }
    }
    if(name.size() <= 4 || name.size() > 255 || !name.ends_with(".ovl") || name.find_first_of("/\\:<>\"|?*") != std::string::npos || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return {};
    return "sdmc:/switch/.overlays/" + name;
}

OverlayUpdater::OverlayUpdater(drive::HttpTransport transport, backup::Control control) : transport(std::move(transport)), control(std::move(control)) {}

bool OverlayUpdater::fetch(drive::HttpRequest request, drive::HttpReply& reply, std::string& error) {
    request.headers = {"User-Agent: SwitchCloudBackup-Updater", "Accept: application/octet-stream"};
    if(request.url.starts_with(Api)) request.headers.back() = "Accept: application/vnd.github+json";
    for(unsigned redirects = 0; redirects <= 5; ++redirects) {
        if(control.stopped()) {
            error = "Update cancelled.";
            return false;
        }
        if(!allowed_url(request.url)) {
            error = "GitHub returned an unexpected update URL.";
            return false;
        }
        control.report(request.operation);
        const backup::Control transfer{control.cancel, [&](const std::string&, uint64_t done, uint64_t total) { control.report(request.operation, done, total); }};
        reply = transport(request, transfer);
        if(reply.transport_ok && (reply.status == 301 || reply.status == 302 || reply.status == 303 || reply.status == 307 || reply.status == 308)) {
            request.url = reply.location;
            continue;
        }
        if(reply.success()) return true;
        error = control.stopped() ? "Update cancelled." : !reply.transport_ok ? request.operation + " failed: " + reply.error
                                                                              : request.operation + " failed (HTTP " + std::to_string(reply.status) + ").";
        if(reply.status == 403 || reply.status == 429) error += " GitHub may be rate limiting requests. Try again later.";
        return false;
    }
    error = "Too many GitHub download redirects.";
    return false;
}

bool OverlayUpdater::releases(unsigned page, std::vector<Release>& out, bool& has_more, std::string& error) {
    out.clear();
    has_more = false;
    if(page == 0 || page > 1000) {
        error = "Invalid release page.";
        return false;
    }
    drive::HttpRequest request;
    request.operation = "Loading release versions ...";
    request.url = std::string(Api) + "?per_page=" + std::to_string(PageSize) + "&page=" + std::to_string(page);
    drive::HttpReply reply;
    if(!fetch(request, reply, error)) return false;
    const auto json = parse(reply.body);
    if(!cJSON_IsArray(json.get()) || cJSON_GetArraySize(json.get()) > static_cast<int>(PageSize)) {
        error = "Could not read GitHub release versions.";
        return false;
    }
    has_more = cJSON_GetArraySize(json.get()) == PageSize && page < 1000;
    const cJSON* item;
    cJSON_ArrayForEach(item, json.get()) {
        Release release;
        if(read_release(item, release)) out.push_back(std::move(release));
    }
    return true;
}

bool OverlayUpdater::install(const std::string& tag, const std::string& target, std::string& installed_tag, std::string& error, const std::function<bool()>& begin_replace) {
    installed_tag.clear();
    if(target.empty() || overlay_path(target) != target || (!tag.empty() && !valid_tag(tag))) {
        error = "The overlay path or selected release version is invalid.";
        return false;
    }
    struct stat info{};
    if(::lstat(target.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        error = "Could not find the running overlay file. Update stopped.";
        return false;
    }
    drive::HttpRequest request;
    request.operation = "Checking GitHub release ...";
    request.url = std::string(Api) + (tag.empty() ? "/latest" : "/tags/" + tag);
    drive::HttpReply reply;
    if(!fetch(request, reply, error)) return false;
    const auto json = parse(reply.body);
    Release release;
    if(!read_release(json.get(), release) || (!tag.empty() && release.tag != tag)) {
        error = "This release has no valid overlay and SHA-256 checksum.";
        return false;
    }
    request.operation = "Downloading release checksum ...";
    request.url = release.checksum_url;
    if(!fetch(request, reply, error)) return false;
    std::string expected;
    if(!checksum(reply.body, expected)) {
        error = "The release checksum file is invalid.";
        return false;
    }

    const auto staged_path = target + ".update";
    if(::lstat(staged_path.c_str(), &info) == 0 && !S_ISREG(info.st_mode)) {
        error = "The update staging path is not a regular file.";
        return false;
    }
    StagedFile staged{staged_path};
    request.operation = "Downloading " + release.tag + " ...";
    request.url = release.overlay_url;
    request.download_path = staged.path;
    request.transfer_size = release.size;
    if(!fetch(request, reply, error) || !validate_overlay(staged.path, release.size, error)) return false;
    control.report("Verifying overlay SHA-256 ...");
    std::string actual;
    if(!backup::hash_file(staged.path, actual, error, control)) return false;
    if(actual != expected) {
        error = "Overlay SHA-256 mismatch. The installed overlay was kept.";
        return false;
    }
    if(control.stopped() || (begin_replace && !begin_replace())) {
        error = "Update cancelled.";
        return false;
    }
    control.report("Installing " + release.tag + " ...");
    // Switch fsdev cannot rename over an existing file. Keep the old file so a
    // failed installation can put it back; it also remains available afterwards.
    const auto previous = target + ".previous";
    if(::unlink(previous.c_str()) != 0 && errno != ENOENT) {
        error = io_error("Could not replace the previous overlay backup", errno);
        return false;
    }
    if(std::rename(target.c_str(), previous.c_str()) != 0) {
        error = io_error("Could not retain the installed overlay", errno);
        return false;
    }
    if(std::rename(staged.path.c_str(), target.c_str()) != 0) {
        const int code = errno;
        error = io_error("Could not install the downloaded overlay", code);
        if(std::rename(previous.c_str(), target.c_str()) != 0) error += " Restore the overlay manually from " + previous + ".";
        return false;
    }
    staged.path.clear();
    installed_tag = release.tag;
    return true;
}

} // namespace updater
