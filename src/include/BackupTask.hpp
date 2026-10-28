#pragma once

#include "BackupTypes.hpp"
#include "RestoreGuard.hpp"
#include "BackgroundJob.hpp"

#include <switch.h>
#include <memory>
#include <mutex>
#include <vector>

namespace backup {

class BackupTask {
public:
    struct View {
        bool busy = false;
        bool critical = false;
        bool question = false;
        bool deleted = false;

        std::string message;
        std::string current_hash;
        std::string recovery_path;

        uint64_t done = 0;
        uint64_t total = 0;

        std::shared_ptr<const std::vector<CloudBackup>> entries;
    };

    BackupTask(AccountUid uid, u64 application_id, std::string user_name, std::string application_name);
    ~BackupTask();
    BackupTask(const BackupTask&) = delete;
    BackupTask& operator=(const BackupTask&) = delete;

    void backup();
    void list(std::shared_ptr<const std::vector<CloudBackup>> entries = {});
    void prepare_restore(CloudBackup entry);
    void continue_restore(RestoreChoice choice);
    void delete_backup(CloudBackup entry);
    void cancel();
    void reap_finished();
    View view() const;

private:
    enum class Job { Backup,
        List,
        Restore,
        Delete };

    static void thread_entry(void* context);
    void start(Job job);
    void run();
    bool execute(std::string& error);
    void ask(const std::string& hash);
    void report(const std::string& message, uint64_t done = 0, uint64_t total = 0);

    AccountUid uid;
    u64 application_id;
    Identity identity;

    mutable std::mutex mutex;
    View current;

    std::atomic<bool> cancelled{false};
    background::Job worker;

    Job job = Job::List;
    CloudBackup selected;
    RestoreChoice choice = RestoreChoice::Check;
    std::string confirmed_hash;
};

} // namespace backup
