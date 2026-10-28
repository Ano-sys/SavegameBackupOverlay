#include "include/BackupTask.hpp"
#include "include/DriveStorage.hpp"
#include "include/JsonHandler.hpp"
#include "include/SaveArchive.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <cstdio>
#include <sys/stat.h>
#include <string_view>
#include <set>

namespace fs = std::filesystem;

namespace {
const AccountUid User{{1, 2}};
constexpr u64 Application = 42;
const backup::Identity Identity{"00000000000000010000000000000002", "000000000000002A", "Timo", "Test Game", "OLED"};
fs::path local_save;
int commits = 0, uploads = 0, refreshes = 0, mounts = 0, deletions = 0;
bool fail_upload = false, fail_list = false, fail_commit = false, corrupt_download = false;
bool running = false, unauthorized_once = false;
bool fail_delete = false, unauthorized_patch_once = false;
int directory_error = 0, directory_collisions = 0;
std::optional<drive::HttpReply> forced_reply;
std::optional<drive::HttpReply> forced_refresh_reply;
std::vector<fs::path> collided_directories;
std::function<void()> after_upload;
std::function<void()> after_download;
std::function<void()> after_list;
struct RemoteFile {
    backup::CloudBackup entry;
    std::string bytes;
    backup::Identity identity = Identity;
    bool trashed = false;
};
std::vector<RemoteFile> remote_files;
std::string pending_metadata;
std::vector<Result> commit_results;
std::vector<int> restore_open_errors;
s64 journal_size = 256 * 1024;
Result journal_result = 0;
fs::path committed_save;
bool writable_mount = false, transaction_failed = false;
std::set<FILE*> save_writers;
uint64_t pending_bytes = 0, largest_transaction = 0;
std::optional<uint64_t> write_failure_after;
int discarded_transactions = 0;

bool journal_reserve(uint64_t bytes) {
    if(transaction_failed || pending_bytes + bytes > static_cast<uint64_t>(journal_size) / 2) {
        transaction_failed = true;
        errno = EIO;
        return false;
    }
    pending_bytes += bytes;
    largest_transaction = std::max(largest_transaction, pending_bytes);
    return true;
}

void copy_save(const fs::path& source, const fs::path& destination) {
    fs::remove_all(destination);
    fs::copy(source, destination, fs::copy_options::recursive);
}

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), bytes.size());
    file.close();
    assert(file.good());
}

std::string encode_json(cJSON* json) {
    char* text = cJSON_PrintUnformatted(json);
    assert(text);
    std::string result(text);
    cJSON_free(text);
    cJSON_Delete(json);
    return result;
}

cJSON* metadata(const backup::CloudBackup& entry, const backup::Identity& identity = Identity) {
    auto* file = cJSON_CreateObject();
    JsonHandler::set_string(file, "id", entry.id);
    JsonHandler::set_string(file, "name", entry.name);
    JsonHandler::set_string(file, "createdTime", entry.created_time);
    JsonHandler::set_string(file, "size", std::to_string(entry.size));
    auto* properties = cJSON_AddObjectToObject(file, "appProperties");
    JsonHandler::set_string(properties, "backup_format", "1");
    JsonHandler::set_string(properties, "user_id", identity.user_id);
    JsonHandler::set_string(properties, "user_name", identity.user_name);
    JsonHandler::set_string(properties, "console_name", identity.console_name);
    JsonHandler::set_string(properties, "application_id", identity.application_id);
    JsonHandler::set_string(properties, "save_hash", entry.save_hash);
    JsonHandler::set_string(properties, "archive_hash", entry.archive_hash);
    return file;
}

void set_save(const std::string& contents) {
    fs::remove_all(local_save);
    fs::create_directories(local_save / "empty");
    write_file(local_save / "nested/slot", contents);
}

std::string local_hash() {
    std::string hash, error;
    assert(backup::hash_save(local_save, hash, error));
    return hash;
}

backup::BackupTask::View wait_for_task(backup::BackupTask& task) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while(task.view().busy && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto result = task.view();
    assert(!result.busy);
    task.reap_finished();
    task.reap_finished();
    assert(test_open_threads == 1);
    return result;
}

drive::HttpReply transport(const drive::HttpRequest& request, const backup::Control&) {
    if(forced_reply) return *forced_reply;
    if(request.url == "https://oauth2.googleapis.com/token") {
        if(forced_refresh_reply) return *forced_refresh_reply;
        assert(request.body.find("client_id=test-build-client.apps.googleusercontent.com") != std::string::npos);
        assert(request.body.find("client_secret=test-build-secret") != std::string::npos);
        ++refreshes;
        return {true, 200, R"({"access_token":"refreshed","expires_in":3600,"token_type":"Bearer"})", ""};
    }

    if(unauthorized_once) {
        unauthorized_once = false;
        return {true, 401, "", ""};
    }

    if(request.method == "POST" && request.url.find("uploadType=resumable") != std::string::npos) {
        pending_metadata = request.body;
        return {true, 200, "", "https://www.googleapis.com/upload/test-session"};
    }

    if(request.method == "PUT") {
        if(fail_upload) return {true, 503, "", ""};

        auto* json = cJSON_Parse(pending_metadata.c_str());
        auto* properties = cJSON_GetObjectItemCaseSensitive(json, "appProperties");
        RemoteFile file;
        file.bytes = read_file(request.upload_path);
        file.entry = {"uploaded-" + std::to_string(++uploads), JsonHandler::string(json, "name"), "2026-10-25T12:00:00Z", JsonHandler::string(properties, "save_hash"), JsonHandler::string(properties, "archive_hash"), file.bytes.size()};
        file.entry.source = file.identity;
        cJSON_Delete(json);
        remote_files.push_back(file);

        if(after_upload) after_upload();

        return {true, 200, encode_json(metadata(file.entry)), ""};
    }

    if(request.method == "PATCH") {
        if(unauthorized_patch_once) {
            unauthorized_patch_once = false;
            return {true, 401, "", ""};
        }

        if(fail_delete) return {true, 503, "", ""};

        auto* body = cJSON_Parse(request.body.c_str());
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "trashed")));
        assert(cJSON_GetArraySize(body) == 1);
        cJSON_Delete(body);

        for(auto& file : remote_files) {
            if(request.url.find("/" + file.entry.id + "?") == std::string::npos) continue;

            file.trashed = true;
            ++deletions;
            auto* result = cJSON_CreateObject();
            JsonHandler::set_string(result, "id", file.entry.id);
            cJSON_AddBoolToObject(result, "trashed", true);
            return {true, 200, encode_json(result), ""};
        }

        return {true, 404, "", ""};
    }

    if(request.url.find("alt=media") != std::string::npos) {
        for(const auto& file : remote_files) {
            if(request.url.find("/" + file.entry.id + "?") == std::string::npos) continue;

            write_file(request.download_path, corrupt_download ? "broken ZIP" : file.bytes);

            if(after_download) after_download();

            return {true, 200, "", ""};
        }

        return {true, 404, "", ""};
    }

    if(request.operation == "Find backup folder") {
        auto* json = cJSON_CreateObject();
        auto* files = cJSON_AddArrayToObject(json, "files");
        auto* folder = cJSON_CreateObject();
        JsonHandler::set_string(folder, "id", "save-folder");
        JsonHandler::set_string(folder, "name", request.url.find("user_id") != std::string::npos ? backup::source_name(Identity) : Identity.application_name);
        auto* properties = cJSON_AddObjectToObject(folder, "appProperties");
        JsonHandler::set_string(properties, "user_id", Identity.user_id);
        JsonHandler::set_string(properties, "user_name", Identity.user_name);
        JsonHandler::set_string(properties, "console_name", Identity.console_name);
        cJSON_AddItemToArray(files, folder);
        return {true, 200, encode_json(json), ""};
    }
    if(request.operation == "List backup users") {
        auto* json = cJSON_CreateObject();
        auto* files = cJSON_AddArrayToObject(json, "files");
        std::set<std::string> users;
        for(const auto& remote : remote_files) {
            if(remote.trashed || !users.insert(remote.identity.user_id).second) continue;
            auto* folder = cJSON_CreateObject();
            JsonHandler::set_string(folder, "id", "user-folder-" + std::to_string(users.size()));
            JsonHandler::set_string(folder, "name", backup::source_name(remote.identity));
            auto* properties = cJSON_AddObjectToObject(folder, "appProperties");
            JsonHandler::set_string(properties, "user_id", remote.identity.user_id);
            JsonHandler::set_string(properties, "user_name", remote.identity.user_name);
            JsonHandler::set_string(properties, "console_name", remote.identity.console_name);
            cJSON_AddItemToArray(files, folder);
        }
        return {true, 200, encode_json(json), ""};
    }
    if(request.method == "GET" && request.url.find("pageSize=") != std::string::npos) {
        if(fail_list) return {true, 503, "", ""};

        const bool second_page = request.url.find("pageToken=") != std::string::npos;
        auto* json = cJSON_CreateObject();
        auto* files = cJSON_AddArrayToObject(json, "files");
        std::vector<const RemoteFile*> active;

        for(const auto& file : remote_files) {
            if(!file.trashed) active.push_back(&file);
        }

        for(size_t index = second_page ? 1 : 0; index < active.size() && (second_page || index == 0); ++index) cJSON_AddItemToArray(files, metadata(active[index]->entry, active[index]->identity));

        if(!second_page && active.size() > 1) JsonHandler::set_string(json, "nextPageToken", "second");

        if(after_list) after_list();

        return {true, 200, encode_json(json), ""};
    }

    std::fprintf(stderr, "Unexpected request: %s\n", request.url.c_str());
    assert(false);
    return {};
}
} // namespace

extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __wrap_fopen(const char* path, const char* mode) {
    const bool save_write = std::string_view(path).starts_with("cloudsave:/") && (std::strcmp(mode, "wb") == 0 || std::strcmp(mode, "r+b") == 0);
    if(save_write && transaction_failed) {
        errno = EIO;
        return nullptr;
    }
    if(!restore_open_errors.empty() && save_write && std::strcmp(mode, "wb") == 0) {
        const int code = restore_open_errors.front();
        restore_open_errors.erase(restore_open_errors.begin());
        errno = code;
        return nullptr;
    }
    if(save_write && std::strcmp(mode, "wb") == 0 && !journal_reserve(4096)) return nullptr;
    FILE* file = __real_fopen(path, mode);
    if(file && save_write) save_writers.insert(file);
    return file;
}

extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" size_t __wrap_fwrite(const void* bytes, size_t size, size_t count, FILE* file) {
    if(save_writers.contains(file)) {
        if(write_failure_after) {
            if(size * count > *write_failure_after) {
                write_failure_after.reset();
                transaction_failed = true;
                errno = EIO;
                return 0;
            }
            *write_failure_after -= size * count;
        }
        if(!journal_reserve(size * count)) return 0;
    }
    return __real_fwrite(bytes, size, count, file);
}

extern "C" int __real_fclose(FILE*);
extern "C" int __wrap_fclose(FILE* file) {
    save_writers.erase(file);
    return __real_fclose(file);
}

extern "C" int __real_unlink(const char*);
extern "C" int __wrap_unlink(const char* path) {
    if(std::string_view(path).starts_with("cloudsave:/") && !journal_reserve(4096)) return -1;
    return __real_unlink(path);
}
extern "C" int __real_rmdir(const char*);
extern "C" int __wrap_rmdir(const char* path) {
    if(std::string_view(path).starts_with("cloudsave:/") && !journal_reserve(4096)) return -1;
    return __real_rmdir(path);
}

extern "C" char* __real_mkdtemp(char* pattern);
extern "C" char* __wrap_mkdtemp(char* pattern) {
    // devkitA64 newlib does not implement directory creation in mkdtemp.
    if(std::string(pattern).starts_with("sdmc:")) {
        errno = ENOSYS;
        return nullptr;
    }

    return __real_mkdtemp(pattern);
}

extern "C" int __real_mkdir(const char* path, mode_t mode);
extern "C" int __wrap_mkdir(const char* path, mode_t mode) {
    if(std::string_view(path).starts_with("cloudsave:/") && !journal_reserve(4096)) return -1;
    const fs::path directory(path);
    const auto name = directory.filename().string();

    if(name.starts_with("operation-") || name.starts_with("before-restore-")) {
        if(directory_error) {
            errno = directory_error;
            return -1;
        }

        if(directory_collisions > 0) {
            --directory_collisions;
            assert(__real_mkdir(path, mode) == 0);
            write_file(directory / "keep", "existing directory");
            collided_directories.push_back(directory);
            errno = EEXIST;
            return -1;
        }
    }

    return __real_mkdir(path, mode);
}

namespace drive {
HttpReply CurlSession::request(const HttpRequest& request, const backup::Control& control) const { return transport(request, control); }
} // namespace drive

Result setInitialize() { return 0; }
void setExit() {}
Result setGetDeviceNickname(SetSysDeviceNickName* nickname) {
    std::strcpy(nickname->nickname, Identity.console_name.c_str());
    return 0;
}
Result setsysInitialize() { return 0; }
void setsysExit() {}
Result setsysGetDeviceNickname(SetSysDeviceNickName* nickname) { return setGetDeviceNickname(nickname); }

Result pmdmntInitialize() { return 0; }
void pmdmntExit() {}
Result pmdmntGetApplicationProcessId(u64* process_id) {
    *process_id = 123;
    return running ? 0 : MAKERESULT(15, 1);
}

Result pmdmntGetProgramId(u64* application_id, u64) {
    *application_id = Application;
    return 0;
}

Result fsdevMountSaveData(const char* name, u64 application_id, AccountUid uid) {
    assert(application_id == Application && uid.uid[0] == User.uid[0] && uid.uid[1] == User.uid[1]);
    ++mounts;
    assert(!writable_mount && save_writers.empty());
    writable_mount = true;
    transaction_failed = false;
    pending_bytes = 0;
    copy_save(local_save, committed_save);
    fs::create_directory_symlink(local_save, std::string(name) + ":");
    return 0;
}

Result fsdevMountSaveDataReadOnly(const char* name, u64 application_id, AccountUid uid) {
    assert(application_id == Application && uid.uid[0] == User.uid[0] && uid.uid[1] == User.uid[1]);
    ++mounts;
    assert(!writable_mount);
    fs::create_directory_symlink(local_save, std::string(name) + ":");
    return 0;
}
int fsdevUnmountDevice(const char* name) {
    assert(save_writers.empty());
    if(writable_mount) {
        if(transaction_failed) ++discarded_transactions;
        copy_save(committed_save, local_save);
        writable_mount = false;
        pending_bytes = 0;
        transaction_failed = false;
    }
    return fs::remove(std::string(name) + ":") ? 0 : -1;
}
Result fsdevCommitDevice(const char*) {
    assert(writable_mount && save_writers.empty() && !transaction_failed);
    ++commits;

    if(!commit_results.empty()) {
        const auto result = commit_results.front();
        commit_results.erase(commit_results.begin());
        if(result) {
            transaction_failed = true;
            return result;
        }
    }

    if(fail_commit) {
        fail_commit = false;
        transaction_failed = true;
        return 1;
    }

    copy_save(local_save, committed_save);
    pending_bytes = 0;
    return 0;
}

Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader* reader, FsSaveDataSpaceId) {
    reader->consumed = false;
    return 0;
}
Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader* reader, FsSaveDataInfo* info, size_t, s64* count) {
    *count = reader->consumed ? 0 : 1;
    reader->consumed = true;
    *info = {123, FsSaveDataSpaceId_User, FsSaveDataType_Account, User, Application, 0, 0};
    return 0;
}
void fsSaveDataInfoReaderClose(FsSaveDataInfoReader*) {}
Result fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(void* data, size_t size, FsSaveDataSpaceId space, u64 id) {
    assert(size == sizeof(FsSaveDataExtraData) && space == FsSaveDataSpaceId_User && id == 123);
    static_cast<FsSaveDataExtraData*>(data)->journal_size = journal_size;
    return journal_result;
}

int main() {
    char directory[] = "/tmp/sbo-task-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path previous = fs::current_path(), root(directory);
    fs::current_path(root);
    local_save = root / "save";
    committed_save = root / "committed-save";
    fs::create_directory("sdmc:");
    drive::GoogleDriveConfig config;
    std::string error;
    assert(config.save_connection("global", {"initial-access-token", "initial-refresh-token", drive::Scope, static_cast<int64_t>(std::time(nullptr)) + 3600}, error));
    JsonHandler json(drive::AuthFile);
    assert(json.load());
    assert(!cJSON_HasObjectItem(json.root(), "oauth"));

    {
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        forced_reply = drive::HttpReply{false, 0, "", "", "curl 60: certificate error. Bearer initial-access-token https://example.test/private-session?upload_id=secret initial-refresh-token"};
        task.list();
        auto failed = wait_for_task(task);
        assert(failed.message.find("Find backup folder (GET)") != std::string::npos);
        assert(failed.message.find("curl 60") != std::string::npos);
        assert(failed.message.find("initial-access-token") == std::string::npos);
        assert(failed.message.find("initial-refresh-token") == std::string::npos);
        assert(failed.message.find("private-session") == std::string::npos);
        assert(read_file(fs::path(backup::DataDirectory) / "error.log").find(failed.message) != std::string::npos);

        forced_reply = drive::HttpReply{true, 403, R"({"error":{"errors":[{"reason":"accessNotConfigured"}],"message":"Drive API is disabled."}})", ""};
        task.list();
        failed = wait_for_task(task);
        assert(failed.message.find("HTTP 403 [accessNotConfigured]") != std::string::npos);
        assert(failed.message.find("Drive API is disabled.") != std::string::npos);

        forced_reply = drive::HttpReply{true, 403, R"({"error":{"details":[{"reason":"SERVICE_DISABLED"}],"message":"API disabled."}})", ""};
        task.list();
        assert(wait_for_task(task).message.find("SERVICE_DISABLED") != std::string::npos);
        forced_reply.reset();

        unauthorized_once = true;
        forced_refresh_reply = drive::HttpReply{true, 400, R"({"error":"invalid_grant","error_description":"Expired initial-refresh-token test-build-secret"})", ""};
        task.list();
        failed = wait_for_task(task);
        assert(failed.message.find("Refresh Google Drive token") != std::string::npos);
        assert(failed.message.find("invalid_grant") != std::string::npos);
        assert(failed.message.find("initial-refresh-token") == std::string::npos);
        assert(failed.message.find("test-build-secret") == std::string::npos);
        forced_refresh_reply.reset();
    }

    {
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.list();
        assert(wait_for_task(task).entries);
        const auto creations = test_thread_creations;
        test_thread_create_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        backup::BackupTask next_page(User, Application, Identity.user_name, Identity.application_name);
        next_page.list();
        assert(wait_for_task(next_page).entries && test_thread_creations == creations);
        test_thread_create_result = 0;
    }

    set_save("old cloud contents");
    {
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);

        for(int code : {EACCES, ENOSPC}) {
            directory_error = code;
            task.backup();
            const auto failed = wait_for_task(task);
            assert(failed.message.find(std::strerror(code)) != std::string::npos);
            assert(uploads == 0 && mounts == 0 && commits == 0);
        }

        directory_error = 0;
    }

    directory_collisions = 2;
    const auto old_hash = local_hash();
    {
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.backup();
        assert(wait_for_task(task).message == "Backup uploaded to Google Drive.");
    }

    const auto target = remote_files.front().entry;
    assert(directory_collisions == 0 && collided_directories.size() == 2);
    for(const auto& directory : collided_directories) assert(read_file(directory / "keep") == "existing directory");
    assert(target.save_hash == old_hash && uploads == 1 && commits == 0);
    set_save("current local contents");
    const auto initial_hash = local_hash();
    {
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question && commits == 0);
        fail_upload = true;
        task.continue_restore(backup::RestoreChoice::BackupFirst);
        assert(!wait_for_task(task).question && local_hash() == initial_hash && commits == 0);
        fail_upload = false;
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        task.continue_restore(backup::RestoreChoice::BackupFirst);
        const auto restored = wait_for_task(task);
        assert(restored.message.starts_with("Backup restored.") && !restored.question);
        // Three removals, two directory creations, file creation and its data.
        assert(local_hash() == old_hash && uploads == 2 && commits == 7);
        assert(remote_files.back().entry.save_hash == initial_hash);
        assert(fs::exists(restored.recovery_path));
    }

    {
        // The matching hash is on page two, so no extra backup question is allowed.
        set_save("current local contents");
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        const auto restored = wait_for_task(task);
        assert(!restored.question && local_hash() == old_hash && uploads == 2);
    }

    {
        set_save("unbacked contents");
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        const auto question = wait_for_task(task);
        assert(question.question);
        set_save("changed after question");
        const auto changed = local_hash();
        const int before = commits;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto repeated = wait_for_task(task);
        assert(repeated.question && repeated.current_hash == changed && commits == before);
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        assert(wait_for_task(task).message.starts_with("Backup restored."));
        assert(local_hash() == old_hash && uploads == 2);
    }

    {
        set_save("changed during upload");
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        const int before = commits;
        after_upload = [] { set_save("changed again during upload"); };
        task.continue_restore(backup::RestoreChoice::BackupFirst);
        assert(wait_for_task(task).question && commits == before);
        assert(read_file(local_save / "nested/slot") == "changed again during upload");
        after_upload = {};
    }

    {
        set_save("rollback original");
        const auto original = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        fail_commit = true;
        const int before = commits;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto recovered = wait_for_task(task);
        assert(recovered.message.starts_with("Restore failed. The original save was recovered."));
        assert(recovered.message.find("Could not commit restored save 'cloudsave:/nested/slot'") != std::string::npos);
        assert(recovered.message.find("last FS Result 0x00000001") != std::string::npos);
        assert(local_hash() == original && commits == before + 8 && fs::exists(recovered.recovery_path));
    }

    {
        // The file is much larger than the simulated journal. Old restores
        // exhaust it before their first commit, even with no transport failure.
        const std::string bytes(2 * 1024 * 1024 + 137, 'L');
        set_save(bytes);
        write_file(local_save / "zero", "");
        const auto large_hash = local_hash();
        const auto archive = root / "large-backup.zip";
        std::string hash, archive_hash;
        assert(backup::create_archive(local_save, archive, Identity, hash, error));
        assert(backup::hash_file(archive, archive_hash, error));
        auto large_target = target;
        large_target.id = "large-backup";
        large_target.save_hash = hash;
        large_target.archive_hash = archive_hash;
        const auto archive_bytes = read_file(archive);
        large_target.size = archive_bytes.size();
        remote_files.push_back({large_target, archive_bytes});

        set_save("before large restore");
        for(int i = 0; i < 40; ++i) write_file(local_save / ("obsolete-" + std::to_string(i)), "");
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(large_target);
        assert(wait_for_task(task).question);
        largest_transaction = 0;
        const int before = commits;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto restored = wait_for_task(task);
        assert(restored.message.starts_with("Backup restored."));
        assert(local_hash() == large_hash && read_file(local_save / "nested/slot") == bytes);
        assert(largest_transaction <= static_cast<uint64_t>(journal_size) / 4);
        assert(commits > before + 32 && save_writers.empty() && !writable_mount);

        // Fail only after at least one data checkpoint was committed. Recovery
        // must discard the failed transaction and replace the partial save.
        set_save(std::string(192 * 1024 + 13, 'R'));
        const auto original = local_hash();
        task.prepare_restore(large_target);
        assert(wait_for_task(task).question);
        const int discarded = discarded_transactions;
        write_failure_after = 80 * 1024;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto recovered = wait_for_task(task);
        assert(recovered.message.starts_with("Restore failed. The original save was recovered."));
        assert(recovered.message.find("Could not write restore destination") != std::string::npos);
        assert(discarded_transactions == discarded + 1 && !write_failure_after);
        assert(local_hash() == original && fs::exists(recovered.recovery_path));
        assert(save_writers.empty() && !writable_mount);

        // A failed checkpoint after partial progress must also reopen the save.
        task.prepare_restore(large_target);
        assert(wait_for_task(task).question);
        commit_results.assign(7, 0);
        commit_results.push_back(0xC0346402);
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto commit_recovered = wait_for_task(task);
        assert(commit_recovered.message.starts_with("Restore failed. The original save was recovered."));
        assert(commit_recovered.message.find("0xC0346402") != std::string::npos);
        assert(local_hash() == original && commit_results.empty());
        remote_files.pop_back();
    }

    {
        set_save("journal metadata failure");
        const auto original = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        const int before = commits;
        journal_result = 0x1234;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        assert(wait_for_task(task).message.find("Could not read save journal size") != std::string::npos);
        assert(local_hash() == original && commits == before);
        journal_result = 0;
        journal_size = 4096;
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        assert(wait_for_task(task).message.find("journal is too small") != std::string::npos);
        assert(local_hash() == original && commits == before);
        journal_size = 256 * 1024;
    }

    {
        set_save("commit failure original");
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        commit_results = {0x0000c802, 0x0000ca02};
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto failed = wait_for_task(task);
        assert(failed.message.starts_with("Restore and rollback failed."));
        assert(failed.message.find("Restore cause: Could not commit restored save") != std::string::npos);
        assert(failed.message.find("Rollback cause: Could not commit recovered save") != std::string::npos);
        assert(failed.message.find("0x0000C802") != std::string::npos);
        assert(failed.message.find("0x0000CA02") != std::string::npos);
        assert(failed.message.find(failed.recovery_path) != std::string::npos);
        assert(commit_results.empty() && fs::exists(failed.recovery_path));
        const auto log = read_file(fs::path(backup::DataDirectory) / "error.log");
        assert(log.find("Restore failure: Could not commit restored save") != std::string::npos);
        assert(log.find("Rollback failure: Could not commit recovered save") != std::string::npos);
    }

    {
        set_save("I/O failure original");
        const auto original = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        restore_open_errors = {EACCES, ENOSPC};
        const int before = commits;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        const auto failed = wait_for_task(task);
        assert(failed.message.starts_with("Restore and rollback failed."));
        assert(failed.message.find("Restore cause: Could not open restore destination 'cloudsave:/nested/slot'") != std::string::npos);
        assert(failed.message.find("Rollback cause: Could not open restore destination 'cloudsave:/nested/slot'") != std::string::npos);
        assert(failed.message.find(std::strerror(EACCES)) != std::string::npos);
        assert(failed.message.find(std::strerror(ENOSPC)) != std::string::npos);
        assert(commits > before && restore_open_errors.empty());
        assert(fs::exists(failed.recovery_path));
        std::string recovery_hash;
        assert(backup::hash_file(failed.recovery_path, recovery_hash, error));
        assert(backup::extract_archive(failed.recovery_path, root / "verified-recovery", Identity, original, error));
    }

    {
        set_save("corrupt download original");
        const auto original = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        corrupt_download = true;
        const int before = commits;
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        assert(!wait_for_task(task).question && local_hash() == original && commits == before);
        corrupt_download = false;
        task.prepare_restore(target);
        assert(wait_for_task(task).question);
        after_download = [&task] { task.cancel(); };
        task.continue_restore(backup::RestoreChoice::WithoutBackup);
        assert(!wait_for_task(task).question && local_hash() == original && commits == before);
        after_download = {};
        fail_list = true;
        task.prepare_restore(target);
        assert(!wait_for_task(task).question && local_hash() == original && commits == before);
        fail_list = false;
        running = true;
        task.prepare_restore(target);
        assert(wait_for_task(task).message.starts_with("Close this game"));
        assert(local_hash() == original && commits == before);
        running = false;
        unauthorized_once = true;
        task.list();
        assert(wait_for_task(task).entries && refreshes == 1);
        assert(json.load());
        assert(!cJSON_HasObjectItem(json.root(), "oauth"));
    }

    {
        set_save("save to delete from the cloud");
        const auto original = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.backup();
        assert(wait_for_task(task).message == "Backup uploaded to Google Drive.");
        const auto selected = remote_files.back().entry;

        RemoteFile other_user = remote_files.back();
        other_user.entry.id = "foreign-user";
        other_user.identity.user_id = "other-user";
        remote_files.push_back(other_user);
        RemoteFile other_game = remote_files.back();
        other_game.entry.id = "foreign-game";
        other_game.identity = {Identity.user_id, "other-game"};
        remote_files.push_back(other_game);

        const int previous_mounts = mounts, previous_commits = commits;
        task.delete_backup(other_user.entry);
        assert(!wait_for_task(task).deleted && deletions == 0);
        task.delete_backup(other_game.entry);
        assert(!wait_for_task(task).deleted && deletions == 0);
        auto changed = selected;
        changed.archive_hash = std::string(64, '0');
        task.delete_backup(changed);
        assert(!wait_for_task(task).deleted && deletions == 0);

        fail_list = true;
        task.delete_backup(selected);
        assert(!wait_for_task(task).deleted && deletions == 0);
        fail_list = false;
        fail_delete = true;
        task.delete_backup(selected);
        assert(!wait_for_task(task).deleted && deletions == 0);
        fail_delete = false;
        after_list = [&task] { task.cancel(); };
        task.delete_backup(selected);
        assert(!wait_for_task(task).deleted && deletions == 0);
        after_list = {};

        running = true;
        unauthorized_patch_once = true;
        const int previous_refreshes = refreshes;
        task.delete_backup(selected);
        const auto deleted = wait_for_task(task);
        assert(deleted.deleted && deleted.message == "Backup moved to Google Drive trash.");
        assert(deletions == 1 && refreshes == previous_refreshes + 1);
        assert(mounts == previous_mounts && commits == previous_commits && local_hash() == original);

        for(const auto& file : remote_files) assert(file.trashed == (file.entry.id == selected.id));
        task.list();
        const auto listed = wait_for_task(task);
        assert(listed.entries && !listed.deleted);

        for(const auto& entry : *listed.entries) assert(entry.id != selected.id);
        task.delete_backup(selected);
        assert(!wait_for_task(task).deleted && deletions == 1);

        running = false;
        task.prepare_restore(target);
        assert(wait_for_task(task).question && local_hash() == original && commits == previous_commits);
    }

    {
        remote_files.clear();
        set_save("Erista save for a user with a different UID");
        const auto foreign_hash = local_hash();
        const backup::Identity foreign{"erista-user-uid", Identity.application_id, Identity.user_name, Identity.application_name, "Erista"};
        std::string hash, error, archive_hash;
        const auto archive = root / "erista.zip";
        assert(backup::create_archive(local_save, archive, foreign, hash, error));
        assert(backup::hash_file(archive, archive_hash, error));
        RemoteFile file;
        file.identity = foreign;
        file.bytes = read_file(archive);
        file.entry = {"erista-backup", "erista.zip", "2026-10-28T12:00:00Z", hash, archive_hash, file.bytes.size(), foreign};
        remote_files.push_back(file);

        set_save("OLED destination save must be backed up first");
        const auto destination_hash = local_hash();
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.list();
        const auto listed = wait_for_task(task);
        assert(listed.entries && listed.entries->size() == 1);
        const auto selected = listed.entries->front();
        assert(selected.source.user_id == foreign.user_id && selected.source.console_name == "Erista");

        auto wrong_game = selected;
        wrong_game.source.application_id = "other-game";
        task.prepare_restore(wrong_game);
        assert(wait_for_task(task).message.find("different game") != std::string::npos);
        assert(local_hash() == destination_hash);

        task.prepare_restore(selected);
        assert(wait_for_task(task).question && local_hash() == destination_hash);
        const auto previous_uploads = uploads;
        task.continue_restore(backup::RestoreChoice::BackupFirst);
        const auto restored = wait_for_task(task);
        assert(!restored.question && local_hash() == foreign_hash && uploads == previous_uploads + 1);
        assert(remote_files.back().identity.user_id == Identity.user_id);
        assert(remote_files.back().entry.source.console_name == "OLED");
        assert(backup::extract_archive(restored.recovery_path, root / "oled-recovery", Identity, destination_hash, error));

        // The same explicit source also scopes deletion to the foreign backup.
        task.delete_backup(selected);
        assert(wait_for_task(task).deleted && remote_files.front().trashed && !remote_files.back().trashed);
    }

    {
        // Opening the source user's save list must work even when another
        // background stack cannot be allocated. It shares the loaded snapshot.
        auto entries = std::make_shared<const std::vector<backup::CloudBackup>>(std::vector<backup::CloudBackup>{remote_files.back().entry});
        const auto creations = test_thread_creations;
        const auto previous_mounts = mounts;
        test_thread_create_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        backup::BackupTask task(User, Application, Identity.user_name, Identity.application_name);
        task.list(entries);
        const auto cached = task.view();
        assert(!cached.busy && cached.entries == entries);
        assert(cached.message == "Select a cloud backup.");
        assert(test_thread_creations == creations && test_open_threads == 1 && mounts == previous_mounts);

        // Refresh and backup keep working with no capacity for a new stack.
        task.list();
        assert(wait_for_task(task).entries);
        task.backup();
        assert(wait_for_task(task).message == "Backup uploaded to Google Drive.");
        assert(test_thread_creations == creations && test_open_threads == 1);
        test_thread_create_result = 0;
    }

    std::string worker_error;
    assert(background::shutdown(worker_error) && test_open_threads == 0);

    fs::current_path(previous);
    fs::remove_all(root);
    std::puts("Shared worker survives failed stack allocation, cached lists, cross-console restore, backup, deletion, recovery, cancellation, and rollback passed.");
}
