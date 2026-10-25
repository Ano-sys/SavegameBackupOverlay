#include "include/BackupTask.hpp"
#include "include/DriveStorage.hpp"
#include "include/SaveArchive.hpp"
#include "include/Diagnostics.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace backup {
namespace {
std::mutex operation_mutex;

bool game_closed(u64 application_id, std::string& error) {
    Result rc = pmdmntInitialize();

    if(R_FAILED(rc)) {
        error = "Could not check running applications.";
        return false;
    }

    u64 process_id = 0, running_id = 0;
    rc = pmdmntGetApplicationProcessId(&process_id);

    // pm returns ProcessNotFound when no application is running.

    if(rc == MAKERESULT(15, 1)) {
        pmdmntExit();
        return true;
    }

    if(R_SUCCEEDED(rc)) rc = pmdmntGetProgramId(&running_id, process_id);

    pmdmntExit();

    if(R_FAILED(rc)) {
        error = "Could not identify the running application.";
        return false;
    }

    if(running_id == application_id) {
        error = "Close this game before backing up or restoring its save.";
        return false;
    }

    return true;
}

class SaveMount {
public:
    ~SaveMount() { close(); }
    bool open(AccountUid uid, u64 application_id, bool writable, std::string& error) {
        const Result rc = writable ? fsdevMountSaveData("cloudsave", application_id, uid) : fsdevMountSaveDataReadOnly("cloudsave", application_id, uid);
        mounted = R_SUCCEEDED(rc);

        if(!mounted) error = "Could not mount the game's account save (" + std::to_string(rc) + ").";

        return mounted;
    }

    bool close() {
        if(!mounted) return true;

        if(fsdevUnmountDevice("cloudsave") != 0) return false;
        mounted = false;
        return true;
    }

private:
    bool mounted = false;
};

bool restore_write_limit(AccountUid uid, u64 application_id, uint64_t& limit, std::string& error) {
    FsSaveDataInfoReader reader{};
    Result result = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);
    if(R_FAILED(result)) {
        error = filesystem_error("Could not open save journal metadata", "cloudsave:/", 0, result);
        return false;
    }

    FsSaveDataInfo info{};
    bool found = false;
    while(true) {
        s64 count = 0;
        result = fsSaveDataInfoReaderRead(&reader, &info, 1, &count);
        if(R_FAILED(result) || count == 0) break;
        if(info.save_data_type == FsSaveDataType_Account && info.application_id == application_id && info.uid.uid[0] == uid.uid[0] && info.uid.uid[1] == uid.uid[1] && info.save_data_index == 0 && info.save_data_rank == 0) {
            found = true;
            break;
        }
    }
    fsSaveDataInfoReaderClose(&reader);
    if(R_FAILED(result)) {
        error = filesystem_error("Could not read save journal metadata", "cloudsave:/", 0, result);
        return false;
    }
    if(!found) {
        error = "Could not find the account save's journal metadata. Restore stopped before writing.";
        return false;
    }

    FsSaveDataExtraData extra{};
    result = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), static_cast<FsSaveDataSpaceId>(info.save_data_space_id), info.save_data_id);
    if(R_FAILED(result)) {
        error = filesystem_error("Could not read save journal size", "cloudsave:/", 0, result);
        return false;
    }
    if(extra.journal_size <= 0) {
        error = "Save journal size is unavailable. Restore stopped before writing.";
        return false;
    }

    // Leave room for allocation and integrity metadata. Align the limit to a
    // save storage block, and cap large journals to keep transactions bounded.
    limit = std::min<uint64_t>(1024 * 1024, static_cast<uint64_t>(extra.journal_size) / 4) & ~uint64_t{4095};
    if(limit == 0) {
        error = "Save journal is too small for a bounded restore. Restore stopped before writing.";
        return false;
    }
    return true;
}

bool unique_directory(const std::string& parent, const char* prefix, const char* description, std::string& path, std::string& error) {
    if(!make_directories(parent, error)) return false;

    // newlib's mkdtemp returns ENOSYS on Switch. Claim the directory with mkdir
    // instead, and never reuse a directory left by an earlier operation.
    static std::atomic<uint64_t> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();

    for(unsigned attempt = 0; attempt < 128; ++attempt) {
        char suffix[34];
        std::snprintf(suffix, sizeof(suffix), "%016llx-%016llx", static_cast<unsigned long long>(tick), static_cast<unsigned long long>(sequence.fetch_add(1, std::memory_order_relaxed)));
        const auto candidate = parent + "/" + prefix + suffix;

        if(::mkdir(candidate.c_str(), 0700) == 0) {
            path = candidate;
            return true;
        }

        const int code = errno;

        if(code != EEXIST) {
            error = std::string("Could not create ") + description + " directory: " + std::strerror(code) + " (errno " + std::to_string(code) + ").";
            return false;
        }
    }

    error = std::string("Could not create a unique ") + description + " directory. Too many existing names.";
    return false;
}

struct Workspace {
    std::string path;
    ~Workspace() {
        if(!path.empty()) {
            std::string ignored;
            remove_tree(path, ignored);
        }
    }

    bool create(std::string& error) {
        const std::string parent = std::string(DataDirectory) + "/tmp";
        return unique_directory(parent, "operation-", "temporary backup", path, error);
    }
};

bool recovery_path(const Identity& identity, std::string& path, std::string& error) {
    const std::string parent = std::string(DataDirectory) + "/recovery/" + identity.user_id + "/" + identity.application_id;

    std::string directory;
    if(!unique_directory(parent, "before-restore-", "recovery backup", directory, error)) return false;

    path = directory + "/save.zip";
    return true;
}
} // namespace

BackupTask::BackupTask(AccountUid uid, u64 application_id, std::string user_name, std::string application_name) : uid(uid), application_id(application_id) {
    char user[33], application[17];
    std::snprintf(user, sizeof(user), "%016llx%016llx", static_cast<unsigned long long>(uid.uid[0]), static_cast<unsigned long long>(uid.uid[1]));
    std::snprintf(application, sizeof(application), "%016llX", static_cast<unsigned long long>(application_id));
    identity = {user, application, std::move(user_name), std::move(application_name)};
}

BackupTask::~BackupTask() {
    cancel();

    if(worker_created) {
        threadWaitForExit(&worker);
        threadClose(&worker);
    }
}

BackupTask::View BackupTask::view() const {
    std::lock_guard lock(mutex);
    return current;
}

void BackupTask::report(const std::string& message, uint64_t done, uint64_t total) {
    std::lock_guard lock(mutex);
    current.message = message;
    current.done = done;
    current.total = total;
}

void BackupTask::ask(const std::string& hash) {
    std::lock_guard lock(mutex);
    current.question = true;
    current.current_hash = hash;
    current.message = "The current save is not backed up on Google Drive. Back it up before restoring?";
    current.done = current.total = 0;
}

void BackupTask::cancel() {
    std::lock_guard lock(mutex);

    if(!current.critical) cancelled.store(true);
}

void BackupTask::backup() {
    if(!view().busy) start(Job::Backup);
}

void BackupTask::list() {
    if(!view().busy) start(Job::List);
}

void BackupTask::prepare_restore(CloudBackup entry) {
    if(view().busy) return;

    selected = std::move(entry);
    choice = RestoreChoice::Check;
    confirmed_hash.clear();
    start(Job::Restore);
}

void BackupTask::continue_restore(RestoreChoice next) {
    const auto state = view();

    if(state.busy || !state.question || next == RestoreChoice::Check) return;

    choice = next;
    confirmed_hash = state.current_hash;
    start(Job::Restore);
}

void BackupTask::delete_backup(CloudBackup entry) {
    if(view().busy) return;

    selected = std::move(entry);
    start(Job::Delete);
}

void BackupTask::start(Job next) {
    if(worker_created && !finished.load()) return;
    reap_finished();
    if(worker_created) return;
    log_memory(next == Job::Backup ? "before backup worker" : next == Job::List ? "before list worker"
                                                          : next == Job::Delete ? "before delete worker"
                                                                                : "before restore worker");

    job = next;
    cancelled.store(false);
    finished.store(false);
    {
        std::lock_guard lock(mutex);
        current.busy = true;
        current.critical = current.question = false;
        current.deleted = false;
        current.message = "Please wait ...";
        current.done = current.total = 0;
    }

    Result rc = threadCreate(&worker, thread_entry, this, nullptr, 0x20000, 0x2C, -2);

    if(R_SUCCEEDED(rc)) {
        worker_created = true;
        rc = threadStart(&worker);

        if(R_FAILED(rc)) {
            threadClose(&worker);
            worker_created = false;
        }
    }

    if(R_FAILED(rc)) {
        std::lock_guard lock(mutex);
        current.busy = false;
        current.message = "Could not start backup worker.";
        finished.store(true);
    }
}

void BackupTask::reap_finished() {
    if(!worker_created || !finished.load()) return;

    Result result = threadWaitForExit(&worker);
    if(R_SUCCEEDED(result)) result = threadClose(&worker);

    if(R_FAILED(result)) {
        const auto message = "Could not release backup worker (Result " + std::to_string(result) + ").";
        if(worker_cleanup_error != result) {
            log_error(message);
            log_memory("after failed worker release");
        }
        worker_cleanup_error = result;
        std::lock_guard lock(mutex);
        current.message = message;
        return;
    }

    worker_cleanup_error = 0;
    worker_created = false;
    log_memory("after worker release");
}

void BackupTask::thread_entry(void* context) { static_cast<BackupTask*>(context)->run(); }

void BackupTask::run() {
    std::unique_lock operation(operation_mutex);
    std::string error;
    const bool ok = execute(error);
    log_memory("after operation cleanup");
    if(!ok) {
        if(error.empty()) error = "Backup operation failed without diagnostic details.";
        error = diagnostic_text(error);
        log_error(error);
    }
    {
        std::lock_guard lock(mutex);

        if(!ok) current.message = error.empty() ? "Backup operation failed." : error;

        current.busy = current.critical = false;
        current.done = current.total = 0;
        finished.store(true);
    }
}

bool BackupTask::execute(std::string& error) {
    const Control control{&cancelled, [this](const std::string& message, uint64_t done, uint64_t total) { report(message, done, total); }};

    if(control.stopped()) {
        error = "Operation cancelled.";
        return false;
    }

    drive::CurlSession network;

    if(!network.ready()) {
        error = network.error().empty() ? "Could not initialize network." : network.error();
        return false;
    }

    drive::GoogleDriveConfig config;
    drive::DriveStorage storage(config, [&](const drive::HttpRequest& request, const Control& transfer) { return network.request(request, transfer); }, control);

    if(!storage.connect("user:" + identity.user_id, error)) return false;
    if(job == Job::List) {
        auto entries = std::make_shared<std::vector<CloudBackup>>();
        report("Loading Google Drive backups ...");

        if(!storage.list(identity, *entries, error)) return false;

        std::lock_guard lock(mutex);
        current.entries = entries;
        current.message = entries->empty() ? "No backups on Google Drive." : "Select a cloud backup.";
        return true;
    }

    if(job == Job::Delete) {
        if(!storage.trash_backup(identity, selected, error)) return false;

        std::lock_guard lock(mutex);
        current.deleted = true;
        current.message = "Backup moved to Google Drive trash.";
        return true;
    }

    if(!game_closed(application_id, error)) return false;

    Workspace workspace;

    if(!workspace.create(error)) return false;
    if(job == Job::Backup) {
        SaveMount save;
        std::string hash, verified;
        const auto archive = workspace.path + "/backup.zip";

        if(!save.open(uid, application_id, false, error) || !create_archive("cloudsave:/", archive, identity, hash, error, control) || !hash_save("cloudsave:/", verified, error, control) || !game_closed(application_id, error)) return false;
        if(hash != verified) {
            error = "Save changed during backup. Close the game and try again.";
            return false;
        }

        if(!save.close()) {
            error = "Could not unmount the save.";
            return false;
        }

        CloudBackup uploaded;

        if(!storage.upload(identity, archive, hash, uploaded, error)) return false;

        report("Backup uploaded to Google Drive.");
        return true;
    }

    std::vector<CloudBackup> backups;
    report("Checking Google Drive backups ...");

    if(!storage.list(identity, backups, error)) return false;

    const auto target = std::find_if(backups.begin(), backups.end(), [&](const CloudBackup& entry) { return entry.id == selected.id; });

    if(target == backups.end()) {
        error = "Selected backup is no longer available. Refresh the list.";
        return false;
    }

    const CloudBackup restore = *target;
    std::string current_hash;
    {
        SaveMount save;

        if(!save.open(uid, application_id, false, error) || !hash_save("cloudsave:/", current_hash, error, control)) return false;
    }

    if(restore_action(current_hash, backups, choice, confirmed_hash) == RestoreAction::Ask) {
        ask(current_hash);
        return true;
    }

    const auto downloaded = workspace.path + "/download.zip";
    const auto staging = workspace.path + "/restore";

    if(!storage.download(restore, downloaded, error) || !extract_archive(downloaded, staging, identity, restore.save_hash, error, control)) return false;

    std::string recovery, snapshot_hash, verified;

    if(!recovery_path(identity, recovery, error)) return false;

    {
        SaveMount save;
        report("Creating local recovery backup ...");

        if(!game_closed(application_id, error) || !save.open(uid, application_id, false, error) || !create_archive("cloudsave:/", recovery, identity, snapshot_hash, error, control) || !hash_save("cloudsave:/", verified, error, control)) return false;
        if(snapshot_hash != verified) {
            error = "Save changed while creating recovery backup. Try again.";
            return false;
        }
    }

    {
        std::lock_guard lock(mutex);
        current.recovery_path = recovery;
    }

    const auto action = restore_action(snapshot_hash, backups, choice, confirmed_hash);

    if(action == RestoreAction::Ask) {
        ask(snapshot_hash);
        return true;
    }

    if(action == RestoreAction::Upload) {
        CloudBackup uploaded;

        if(!storage.upload(identity, recovery, snapshot_hash, uploaded, error)) return false;

        // A successful upload is required before this choice can reach the write step.
        backups.push_back(std::move(uploaded));
    }

    const auto recovery_staging = workspace.path + "/recovery";

    if(!extract_archive(recovery, recovery_staging, identity, snapshot_hash, error, control)) return false;

    uint64_t write_limit = 0;
    if(!restore_write_limit(uid, application_id, write_limit, error)) return false;

    SaveMount save;

    if(!game_closed(application_id, error) || !save.open(uid, application_id, true, error) || !hash_save("cloudsave:/", verified, error, control) || !game_closed(application_id, error)) return false;
    if(verified != snapshot_hash) {
        if(restore_action(verified, backups, RestoreChoice::Check, "") == RestoreAction::Ask) {
            ask(verified);
            return true;
        }

        error = "Save changed before restore. Select the backup again.";
        return false;
    }

    {
        std::lock_guard lock(mutex);

        if(cancelled.load()) {
            error = "Operation cancelled.";
            return false;
        }

        current.critical = true;
    }

    // Intermediate commits can persist a partial save. The verified recovery
    // ZIP remains available, and cancellation stays disabled through recovery.
    const Control critical{nullptr, control.progress};
    const auto write_options = [&](const char* phase) {
        return SaveWriteOptions{write_limit, [phase](const std::string& path, std::string& failure) {
                                    const Result result = fsdevCommitDevice("cloudsave");
                                    if(R_SUCCEEDED(result)) return true;
                                    const auto operation = std::string("Could not commit ") + phase + " save";
                                    failure = filesystem_error(operation.c_str(), path, 0, result);
                                    return false;
                                }};
    };
    bool restored = replace_save("cloudsave:/", staging + "/save", error, critical, write_options("restored")) && hash_save("cloudsave:/", verified, error, critical);

    if(restored && verified != restore.save_hash) {
        error = "Restored save 'cloudsave:/' failed SHA-256 verification (expected " + restore.save_hash + ", got " + verified + ").";
        restored = false;
    }
    if(!restored) {
        const auto restore_error = error.empty() ? "Restore failed without diagnostic details." : error;
        log_error("Restore failure: " + restore_error);
        report("Restore failed. Recovering original save ...");
        std::string rollback_error, rollback_hash;
        // Drop the failed transaction before starting the recovery. Its last
        // committed checkpoint may be partial, so always restore the full ZIP.
        errno = 0;
        bool recovered = save.close();
        if(!recovered) rollback_error = filesystem_error("Could not unmount failed restore", "cloudsave:/", errno);
        if(recovered) recovered = save.open(uid, application_id, true, rollback_error) && replace_save("cloudsave:/", recovery_staging + "/save", rollback_error, critical, write_options("recovered")) && hash_save("cloudsave:/", rollback_hash, rollback_error, critical);
        if(recovered && rollback_hash != snapshot_hash) {
            rollback_error = "Recovered save 'cloudsave:/' failed SHA-256 verification (expected " + snapshot_hash + ", got " + rollback_hash + ").";
            recovered = false;
        }
        if(recovered && !save.close()) {
            rollback_error = "Recovered save committed, but unmount failed.";
            recovered = false;
        }
        if(recovered) error = "Restore failed. The original save was recovered. Restore cause: " + restore_error;
        else {
            if(rollback_error.empty()) rollback_error = "Rollback failed without diagnostic details.";
            log_error("Rollback failure: " + rollback_error);
            error = "Restore and rollback failed. Keep the local recovery ZIP. Manual recovery is required. Restore cause: " + restore_error + " Rollback cause: " + rollback_error;
        }
        error += " Recovery ZIP: " + recovery;
        return false;
    }

    if(!save.close()) {
        error = "Save committed, but unmount failed. Keep the recovery ZIP.";
        return false;
    }

    report("Backup restored. Local recovery ZIP retained on SD.");
    return true;
}

} // namespace backup
