#include "include/DriveStorage.hpp"
#include "include/JsonHandler.hpp"
#include "include/SaveArchive.hpp"
#include "include/Diagnostics.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <memory>
#include <set>
#include <sys/stat.h>

namespace drive {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr const char* Files = "https://www.googleapis.com/drive/v3/files";
constexpr const char* Fields = "id,name,createdTime,size,appProperties";

Json parse(const std::string& body) {
    if(body.size() > 2 * 1024 * 1024 || body.find('\0') != std::string::npos) return Json(nullptr, cJSON_Delete);

    return Json(cJSON_ParseWithLengthOpts(body.c_str(), body.size() + 1, nullptr, true), cJSON_Delete);
}

std::string describe_reply(const HttpReply& reply) {
    if(!reply.transport_ok) return reply.error.empty() ? "Transport failed without diagnostic details." : reply.error;

    std::string description = "HTTP " + std::to_string(reply.status);
    const auto json = parse(reply.body);
    const auto* error = cJSON_GetObjectItemCaseSensitive(json.get(), "error");
    const auto* errors = cJSON_GetObjectItemCaseSensitive(error, "errors");
    std::string reason = JsonHandler::string(cJSON_GetArrayItem(errors, 0), "reason");

    if(reason.empty()) {
        const auto* details = cJSON_GetObjectItemCaseSensitive(error, "details");
        const cJSON* detail;
        cJSON_ArrayForEach(detail, details) {
            reason = JsonHandler::string(detail, "reason");
            if(!reason.empty()) break;
        }
    }

    if(cJSON_IsString(error) && error->valuestring) reason = error->valuestring;
    const auto message = cJSON_IsString(error) ? JsonHandler::string(json.get(), "error_description") : JsonHandler::string(error, "message");
    if(!reason.empty()) description += " [" + reason + "]";
    if(!message.empty()) description += ": " + message;
    return description;
}

std::string encode_json(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);

    if(!text) return "";

    std::string result(text);
    cJSON_free(text);
    return result;
}

bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 256 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
}

bool valid_hash(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool read_backup(const cJSON* file, const backup::Identity& identity, backup::CloudBackup& out) {
    const auto* properties = cJSON_GetObjectItemCaseSensitive(file, "appProperties");
    out.id = JsonHandler::string(file, "id");
    out.name = JsonHandler::string(file, "name");
    out.created_time = JsonHandler::string(file, "createdTime");
    out.save_hash = JsonHandler::string(properties, "save_hash");
    out.archive_hash = JsonHandler::string(properties, "archive_hash");
    const std::string size = JsonHandler::string(file, "size");
    const auto converted = std::from_chars(size.data(), size.data() + size.size(), out.size);
    return valid_id(out.id) && !out.name.empty() && !out.created_time.empty() && valid_hash(out.save_hash) && valid_hash(out.archive_hash) && converted.ec == std::errc{} && converted.ptr == size.data() + size.size() && out.size > 0 && out.size <= backup::MaxArchiveBytes && JsonHandler::string(properties, "backup_format") == "1" && JsonHandler::string(properties, "user_id") == identity.user_id && JsonHandler::string(properties, "application_id") == identity.application_id;
}

std::string quoted(const std::string& value) {
    std::string out = "'";

    for(char c : value) {
        if(c == '\'' || c == '\\') out += '\\';

        out += c;
    }

    return out + "'";
}
} // namespace

DriveStorage::DriveStorage(GoogleDriveConfig& config, HttpTransport transport, backup::Control control) : config(config), transport(std::move(transport)), control(std::move(control)) {}

bool DriveStorage::connect(const std::string& connection_key, std::string& error) {
    std::optional<Tokens> selected;

    if(!config.effective_connection(connection_key, selected, key, error)) return false;
    if(!selected) {
        error = "Connect Google Drive in Settings first.";
        return false;
    }

    tokens = *selected;

    if(!config.folder_id(key, root_id, error)) return false;
    if(!root_id.empty() && !valid_id(root_id)) {
        error = "Invalid Drive folder_id in auth.json.";
        return false;
    }

    if(tokens.access_token.empty() || tokens.expires_at <= static_cast<int64_t>(std::time(nullptr)) + 60) return refresh(error);

    return true;
}

bool DriveStorage::refresh(std::string& error) {
    Credentials credentials;

    if(!config.credentials(credentials, error)) return false;

    control.report("Refreshing Google Drive connection ...");
    HttpRequest request;
    request.method = "POST";
    request.operation = "Refresh Google Drive token";
    request.url = "https://oauth2.googleapis.com/token";
    request.headers = {"Content-Type: application/x-www-form-urlencoded"};
    request.body = "client_id=" + form_encode(credentials.client_id) + "&client_secret=" + form_encode(credentials.client_secret) + "&refresh_token=" + form_encode(tokens.refresh_token) + "&grant_type=refresh_token";
    const auto reply = transport(request, control);

    if(!reply.success()) {
        error = control.stopped() ? "Operation cancelled." : backup::diagnostic_text(request.operation + " failed: " + describe_reply(reply), {tokens.access_token, tokens.refresh_token, credentials.client_id, credentials.client_secret});
        return false;
    }

    Tokens refreshed;

    if(!parse_refreshed_tokens(reply.body, tokens, refreshed, error)) return false;
    if(!config.save_connection(key, refreshed, error)) return false;

    tokens = std::move(refreshed);
    return true;
}

bool DriveStorage::request(HttpRequest request, HttpReply& out, std::string& error) {
    if(control.stopped()) {
        error = "Operation cancelled.";
        return false;
    }

    for(unsigned attempt = 0; attempt < 2; ++attempt) {
        HttpRequest authorized = request;
        authorized.headers.push_back("Authorization: Bearer " + tokens.access_token);
        out = transport(authorized, control);

        if(out.transport_ok && out.status == 401 && attempt == 0) {
            if(!refresh(error)) return false;

            continue;
        }

        if(out.success()) return true;

        const auto operation = request.operation.empty() ? "Google Drive request" : request.operation;
        error = control.stopped() ? "Operation cancelled." : backup::diagnostic_text(operation + " (" + request.method + ") failed: " + describe_reply(out), {tokens.access_token, tokens.refresh_token});
        return false;
    }

    return false;
}

bool DriveStorage::folder(const std::string& parent, const std::string& name, bool create, std::string& out, std::string& error, const std::string& property, const std::string& value) {
    out.clear();
    const std::string selector = property.empty() ? "name = " + quoted(name) : "appProperties has { key = " + quoted(property) + " and value = " + quoted(value) + " }";
    const std::string query = "trashed = false and mimeType = 'application/vnd.google-apps.folder' and " + quoted(parent) + " in parents and " + selector;
    HttpReply reply;
    HttpRequest lookup;
    lookup.operation = "Find backup folder";
    lookup.url = std::string(Files) + "?pageSize=100&fields=files(id,name),nextPageToken,incompleteSearch&q=" + form_encode(query);

    if(!request(lookup, reply, error)) return false;

    const auto json = parse(reply.body);
    const auto* files = cJSON_GetObjectItemCaseSensitive(json.get(), "files");

    if(!cJSON_IsArray(files) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(), "incompleteSearch"))) {
        error = "Incomplete folder response from Google Drive.";
        return false;
    }

    if(cJSON_GetArraySize(files) > 1 || !JsonHandler::string(json.get(), "nextPageToken").empty()) {
        error = property.empty() ? "Multiple matching Drive folders. Set a unique folder_id in auth.json." : "Multiple matching Drive folders for this user or game.";
        return false;
    }

    if(cJSON_GetArraySize(files) == 1) {
        const auto* file = cJSON_GetArrayItem(files, 0);
        out = JsonHandler::string(file, "id");

        if(!valid_id(out)) {
            error = "Google Drive returned an invalid folder ID.";
            return false;
        }

        if(!create || property.empty() || JsonHandler::string(file, "name") == name) return true;

        Json metadata(cJSON_CreateObject(), cJSON_Delete);

        if(!metadata || !JsonHandler::set_string(metadata.get(), "name", name)) {
            error = "Could not prepare Drive folder name.";
            return false;
        }

        HttpRequest rename;
        rename.operation = "Rename backup folder";
        rename.method = "PATCH";
        rename.url = std::string(Files) + "/" + out + "?fields=id";
        rename.headers = {"Content-Type: application/json"};
        rename.body = encode_json(metadata.get());

        if(rename.body.empty() || !request(rename, reply, error)) return false;

        const auto renamed = parse(reply.body);

        if(JsonHandler::string(renamed.get(), "id") == out) return true;

        error = "Google Drive returned an unexpected folder ID after renaming.";
        return false;
    }

    if(!create) return true;

    Json metadata(cJSON_CreateObject(), cJSON_Delete);
    cJSON* parents = cJSON_AddArrayToObject(metadata.get(), "parents");

    if(!metadata || !parents || !cJSON_AddItemToArray(parents, cJSON_CreateString(parent.c_str())) || !JsonHandler::set_string(metadata.get(), "name", name) || !JsonHandler::set_string(metadata.get(), "mimeType", "application/vnd.google-apps.folder")) {
        error = "Could not prepare Drive folder metadata.";
        return false;
    }

    if(!property.empty()) {
        auto* properties = cJSON_AddObjectToObject(metadata.get(), "appProperties");

        if(!properties || !JsonHandler::set_string(properties, property.c_str(), value)) {
            error = "Could not prepare Drive folder identity.";
            return false;
        }
    }

    HttpRequest post;
    post.operation = "Create backup folder";
    post.method = "POST";
    post.url = std::string(Files) + "?fields=id";
    post.headers = {"Content-Type: application/json"};
    post.body = encode_json(metadata.get());

    if(post.body.empty() || !request(post, reply, error)) return false;

    const auto created = parse(reply.body);
    out = JsonHandler::string(created.get(), "id");

    if(valid_id(out)) return true;

    error = "Google Drive did not return the created folder ID.";
    return false;
}

bool DriveStorage::save_folder(const backup::Identity& identity, bool create, std::string& out, std::string& error) {
    out.clear();

    if(identity.user_name.empty() || identity.application_name.empty() || identity.user_name.find('\0') != std::string::npos || identity.application_name.find('\0') != std::string::npos) {
        error = "Switch user name or game title is missing or invalid.";
        return false;
    }

    std::string current = root_id;

    if(current.empty() && !folder("root", "SwitchSavegames", create, current, error)) return false;
    if(current.empty()) {
        out.clear();
        return true;
    }

    std::string user, application;

    if(!folder(current, identity.user_name, create, user, error, "user_id", identity.user_id)) return false;
    if(user.empty()) return true;
    if(!folder(user, identity.application_name, create, application, error, "application_id", identity.application_id)) return false;
    if(application.empty()) return true;

    return folder(application, "Savegames", create, out, error);
}

bool DriveStorage::list(const backup::Identity& identity, std::vector<backup::CloudBackup>& out, std::string& error) {
    out.clear();
    std::string parent;

    if(!save_folder(identity, false, parent, error)) return false;
    if(parent.empty()) return true;

    const std::string query = quoted(parent) + " in parents and trashed = false and mimeType = 'application/zip'";
    std::string page;
    std::set<std::string> pages, ids;
    do {
        if(!pages.insert(page).second || pages.size() > 1000) {
            error = "Invalid Drive pagination.";
            return false;
        }

        HttpRequest get;
        get.operation = "List backups";
        get.url = std::string(Files) + "?pageSize=100&orderBy=createdTime%20desc&fields=" + form_encode(std::string("nextPageToken,incompleteSearch,files(") + Fields + ")") + "&q=" + form_encode(query);

        if(!page.empty()) get.url += "&pageToken=" + form_encode(page);

        HttpReply reply;

        if(!request(get, reply, error)) return false;

        const auto json = parse(reply.body);
        const auto* files = cJSON_GetObjectItemCaseSensitive(json.get(), "files");

        if(!cJSON_IsArray(files) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json.get(), "incompleteSearch"))) {
            error = "Could not read the complete Drive backup list.";
            return false;
        }

        const cJSON* file;
        cJSON_ArrayForEach(file, files) {
            backup::CloudBackup entry;

            if(read_backup(file, identity, entry) && ids.insert(entry.id).second) out.push_back(std::move(entry));
        }

        page = JsonHandler::string(json.get(), "nextPageToken");
    } while(!page.empty());
    return true;
}

bool DriveStorage::upload(const backup::Identity& identity, const std::string& path, const std::string& save_hash, backup::CloudBackup& out, std::string& error) {
    struct stat info{};

    if(stat(path.c_str(), &info) != 0 || info.st_size <= 0 || static_cast<uint64_t>(info.st_size) > backup::MaxArchiveBytes || !valid_hash(save_hash)) {
        error = "Invalid backup archive or save hash.";
        return false;
    }

    const uint64_t size = info.st_size;
    std::string archive_hash, parent;

    if(!backup::hash_file(path, archive_hash, error, control) || !save_folder(identity, true, parent, error)) return false;

    char timestamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &utc);
    const std::string name = std::string(timestamp) + ".zip";
    Json metadata(cJSON_CreateObject(), cJSON_Delete);
    auto* parents = cJSON_AddArrayToObject(metadata.get(), "parents");
    auto* properties = cJSON_AddObjectToObject(metadata.get(), "appProperties");

    if(!metadata || !parents || !properties || !cJSON_AddItemToArray(parents, cJSON_CreateString(parent.c_str())) || !JsonHandler::set_string(metadata.get(), "name", name) || !JsonHandler::set_string(metadata.get(), "mimeType", "application/zip") || !JsonHandler::set_string(properties, "backup_format", "1") || !JsonHandler::set_string(properties, "user_id", identity.user_id) || !JsonHandler::set_string(properties, "application_id", identity.application_id) || !JsonHandler::set_string(properties, "save_hash", save_hash) || !JsonHandler::set_string(properties, "archive_hash", archive_hash)) {
        error = "Could not prepare backup metadata.";
        return false;
    }

    HttpRequest begin;
    begin.operation = "Start backup upload";
    begin.method = "POST";
    begin.url = "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable&fields=" + form_encode(Fields);
    begin.headers = {"Content-Type: application/json", "X-Upload-Content-Type: application/zip", "X-Upload-Content-Length: " + std::to_string(size)};
    begin.body = encode_json(metadata.get());
    HttpReply reply;
    control.report("Starting Google Drive upload ...");

    if(begin.body.empty() || !request(begin, reply, error)) return false;

    // Upload credentials must only be sent to the Google upload endpoint.

    if(!reply.location.starts_with("https://www.googleapis.com/upload/") || reply.location.find_first_of("\r\n ") != std::string::npos) {
        error = "Google Drive returned an invalid upload URL.";
        return false;
    }

    HttpRequest upload;
    upload.operation = "Upload backup ZIP";
    upload.method = "PUT";
    upload.url = reply.location;
    upload.upload_path = path;
    upload.transfer_size = size;
    upload.headers = {"Content-Type: application/zip", "Content-Length: " + std::to_string(size)};
    control.report("Uploading complete save ...");

    if(!request(upload, reply, error)) return false;

    const auto result = parse(reply.body);

    if(!read_backup(result.get(), identity, out) || out.save_hash != save_hash || out.archive_hash != archive_hash || out.size != size) {
        error = "Upload response could not be verified. Refresh the backup list before retrying.";
        return false;
    }

    return true;
}

bool DriveStorage::download(const backup::CloudBackup& entry, const std::string& path, std::string& error) {
    if(!valid_id(entry.id) || !valid_hash(entry.archive_hash) || entry.size == 0 || entry.size > backup::MaxArchiveBytes) {
        error = "Invalid cloud backup metadata.";
        return false;
    }

    HttpRequest get;
    get.operation = "Download backup ZIP";
    get.url = std::string(Files) + "/" + entry.id + "?alt=media";
    get.download_path = path;
    get.transfer_size = entry.size;
    HttpReply reply;
    control.report("Downloading backup ...");

    if(!request(get, reply, error)) return false;

    struct stat info{};
    std::string hash;

    if(stat(path.c_str(), &info) != 0 || static_cast<uint64_t>(info.st_size) != entry.size || !backup::hash_file(path, hash, error, control) || hash != entry.archive_hash) {
        if(error.empty()) error = "Downloaded archive failed its size or SHA-256 check.";

        return false;
    }

    return true;
}

bool DriveStorage::trash_backup(const backup::Identity& identity, const backup::CloudBackup& entry, std::string& error) {
    error.clear();

    if(!valid_id(entry.id)) {
        error = "Invalid cloud backup ID.";
        return false;
    }

    std::vector<backup::CloudBackup> backups;
    control.report("Checking selected cloud backup ...");

    if(!list(identity, backups, error)) return false;

    const auto found = std::find_if(backups.begin(), backups.end(), [&](const backup::CloudBackup& backup) { return backup.id == entry.id; });

    if(found == backups.end()) {
        error = "Selected backup is no longer available for this user/game. Refresh the list.";
        return false;
    }

    if(found->save_hash != entry.save_hash || found->archive_hash != entry.archive_hash || found->size != entry.size) {
        error = "Selected backup has changed. Refresh the list before deleting it.";
        return false;
    }

    HttpRequest patch;
    patch.operation = "Move backup to trash";
    patch.method = "PATCH";
    patch.url = std::string(Files) + "/" + entry.id + "?fields=id,trashed";
    patch.headers = {"Content-Type: application/json"};
    patch.body = R"({"trashed":true})";
    HttpReply reply;
    control.report("Moving backup to Google Drive trash ...");

    if(!request(patch, reply, error)) return false;

    const auto result = parse(reply.body);

    if(JsonHandler::string(result.get(), "id") != entry.id || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result.get(), "trashed"))) {
        error = "Could not confirm deletion. Refresh the backup list before retrying.";
        return false;
    }

    return true;
}

} // namespace drive
