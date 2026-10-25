#include "include/DriveStorage.hpp"
#include "include/JsonHandler.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

struct Folder {
    std::string id, parent, name, property, value;
};

std::string encode(cJSON* json) {
    char* encoded = cJSON_PrintUnformatted(json);
    assert(encoded);
    std::string result(encoded);
    cJSON_free(encoded);
    return result;
}

std::string decode(const std::string& text) {
    std::string result;
    for(size_t index = 0; index < text.size(); ++index) {
        if(text[index] == '%') {
            assert(index + 2 < text.size());
            result += static_cast<char>(std::strtoul(text.substr(index + 1, 2).c_str(), nullptr, 16));
            index += 2;
        }
        else result += text[index] == '+' ? ' ' : text[index];
    }
    return result;
}

std::string query_value(const std::string& query, const std::string& field) {
    const auto begin = query.find(field + " = '");
    if(begin == std::string::npos) return {};
    std::string value;
    for(size_t index = begin + field.size() + 4; index < query.size(); ++index) {
        if(query[index] == '\'') return value;
        if(query[index] == '\\') ++index;
        assert(index < query.size());
        value += query[index];
    }
    assert(false);
    return {};
}
} // namespace

int main() {
    namespace fs = std::filesystem;
    char directory[] = "/tmp/sbo-folder-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory), previous = fs::current_path();
    fs::current_path(root);
    fs::create_directory("sdmc:");
    std::ofstream("backup.zip") << "test archive contents";

    const backup::Identity identity{"first-user", "first-game", "Timo's \\ Profil", "Zelda: Tränen / Königreich"};
    std::vector<Folder> folders{
        {"drive-root", "root", "SwitchSavegames", "", ""},
        {"old-user", "drive-root", identity.user_id, "", ""},
        {"old-game", "old-user", identity.application_id, "", ""},
    };
    std::string pending_upload;
    std::vector<std::string> upload_parents;
    unsigned renames = 0, requests = 0;

    drive::GoogleDriveConfig config;
    std::string error;
    assert(config.save_connection("global", {"access", "refresh", drive::Scope, static_cast<int64_t>(std::time(nullptr)) + 3600}, error));

    drive::DriveStorage storage(config, [&](const drive::HttpRequest& request, const backup::Control&) -> drive::HttpReply {
        ++requests;
        if(request.operation == "Find backup folder") {
            const auto query = decode(request.url.substr(request.url.find("&q=") + 3));
            const auto property = query_value(query, "key");
            const auto value = query_value(query, property.empty() ? "name" : "value");
            Json reply(cJSON_CreateObject(), cJSON_Delete);
            auto* files = cJSON_AddArrayToObject(reply.get(), "files");

            for(const auto& folder : folders) {
                if(query.find("'" + folder.parent + "' in parents") == std::string::npos) continue;
                if(property.empty() ? folder.name != value : folder.property != property || folder.value != value) continue;
                auto* file = cJSON_CreateObject();
                JsonHandler::set_string(file, "id", folder.id);
                JsonHandler::set_string(file, "name", folder.name);
                cJSON_AddItemToArray(files, file);
            }
            return {true, 200, encode(reply.get()), ""};
        }

        if(request.operation == "Create backup folder") {
            Json metadata(cJSON_Parse(request.body.c_str()), cJSON_Delete);
            const auto* parents = cJSON_GetObjectItemCaseSensitive(metadata.get(), "parents");
            const auto* properties = cJSON_GetObjectItemCaseSensitive(metadata.get(), "appProperties");
            assert(cJSON_GetArraySize(parents) == 1);
            Folder folder{"folder-" + std::to_string(folders.size()), cJSON_GetArrayItem(parents, 0)->valuestring, JsonHandler::string(metadata.get(), "name"), "", ""};
            for(const char* property : {"user_id", "application_id"}) {
                const auto value = JsonHandler::string(properties, property);
                if(!value.empty()) {
                    folder.property = property;
                    folder.value = value;
                }
            }
            assert(folder.property.empty() ? folder.name == "Savegames" : !folder.value.empty());
            folders.push_back(folder);
            Json reply(cJSON_CreateObject(), cJSON_Delete);
            JsonHandler::set_string(reply.get(), "id", folder.id);
            return {true, 200, encode(reply.get()), ""};
        }

        if(request.operation == "Rename backup folder") {
            Json metadata(cJSON_Parse(request.body.c_str()), cJSON_Delete);
            assert(cJSON_GetArraySize(metadata.get()) == 1);
            for(auto& folder : folders) {
                if(request.url.find("/" + folder.id + "?") == std::string::npos) continue;
                folder.name = JsonHandler::string(metadata.get(), "name");
                ++renames;
                Json reply(cJSON_CreateObject(), cJSON_Delete);
                JsonHandler::set_string(reply.get(), "id", folder.id);
                return {true, 200, encode(reply.get()), ""};
            }
            assert(false);
        }

        if(request.method == "POST" && request.url.find("uploadType=resumable") != std::string::npos) {
            pending_upload = request.body;
            return {true, 200, "", "https://www.googleapis.com/upload/test-session"};
        }

        if(request.method == "PUT") {
            Json metadata(cJSON_Parse(pending_upload.c_str()), cJSON_Delete);
            const auto* parents = cJSON_GetObjectItemCaseSensitive(metadata.get(), "parents");
            upload_parents.emplace_back(cJSON_GetArrayItem(parents, 0)->valuestring);
            JsonHandler::set_string(metadata.get(), "id", "upload-" + std::to_string(upload_parents.size()));
            JsonHandler::set_string(metadata.get(), "createdTime", "2026-10-25T12:00:00Z");
            JsonHandler::set_string(metadata.get(), "size", std::to_string(request.transfer_size));
            return {true, 200, encode(metadata.get()), ""};
        }

        if(request.operation == "List backups") return {true, 200, R"({"files":[]})", ""};
        assert(false);
        return {};
    });
    assert(storage.connect("user:" + identity.user_id, error));

    const auto find_folder = [&](const std::string& parent, const std::string& property, const std::string& value) -> const Folder& {
        const auto folder = std::find_if(folders.begin(), folders.end(), [&](const Folder& candidate) { return candidate.parent == parent && candidate.property == property && candidate.value == value; });
        assert(folder != folders.end());
        return *folder;
    };
    const auto upload = [&](const backup::Identity& selected) {
        backup::CloudBackup entry;
        assert(storage.upload(selected, "backup.zip", std::string(64, 'a'), entry, error));
    };

    upload(identity);
    assert(folders.size() == 6 && renames == 0);
    const auto user_id = find_folder("drive-root", "user_id", identity.user_id).id;
    assert(find_folder("drive-root", "user_id", identity.user_id).name == identity.user_name);
    const auto game_id = find_folder(user_id, "application_id", identity.application_id).id;
    assert(find_folder(user_id, "application_id", identity.application_id).name == identity.application_name);
    const auto save_id = upload_parents.back();
    assert(find_folder(game_id, "", "").id == save_id);

    auto other_user = identity;
    other_user.user_id = "second-user";
    upload(other_user);
    assert(folders.size() == 9 && upload_parents.back() != save_id);
    assert(find_folder("drive-root", "user_id", other_user.user_id).id != user_id);

    auto other_game = identity;
    other_game.application_id = "second-game";
    upload(other_game);
    assert(folders.size() == 11 && upload_parents.back() != save_id);
    assert(find_folder(user_id, "application_id", other_game.application_id).id != game_id);

    auto renamed = identity;
    renamed.user_name = "Renamed user";
    renamed.application_name = "Renamed game";
    std::vector<backup::CloudBackup> entries;
    assert(storage.list(renamed, entries, error) && renames == 0);
    upload(renamed);
    assert(folders.size() == 11 && renames == 2 && upload_parents.back() == save_id);
    assert(find_folder("drive-root", "user_id", identity.user_id).name == renamed.user_name);
    assert(find_folder(user_id, "application_id", identity.application_id).name == renamed.application_name);
    assert(folders[1].name == identity.user_id && folders[2].name == identity.application_id);

    auto duplicate = find_folder("drive-root", "user_id", identity.user_id);
    duplicate.id = "duplicate-user";
    folders.push_back(duplicate);
    assert(!storage.list(identity, entries, error));
    assert(error.find("Multiple matching Drive folders") != std::string::npos);

    auto missing_name = identity;
    missing_name.user_name.clear();
    const auto before = requests;
    assert(!storage.list(missing_name, entries, error) && requests == before);
    assert(error.find("missing or invalid") != std::string::npos);

    fs::current_path(previous);
    fs::remove_all(root);
    std::puts("Named Drive folders, Unicode, duplicate display names, renaming, and ignored legacy folders passed.");
}
