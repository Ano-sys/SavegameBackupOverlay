#include "include/BackgroundJob.hpp"
#include "include/Diagnostics.hpp"

#include <switch.h>
#include <condition_variable>
#include <mutex>

namespace background {

struct Worker {
    std::mutex mutex;
    std::condition_variable wake;
    Thread thread{};
    bool created = false;
    bool started = false;
    bool stopping = false;
    Job* head = nullptr;
    Job* tail = nullptr;

    ~Worker() {
        std::string error;
        if(!shutdown(error)) backup::log_error(error);
    }

    static void entry(void* context) { static_cast<Worker*>(context)->run(); }

    bool start_locked(std::string& error) {
        if(stopping) {
            error = "The background worker is shutting down or could not be released.";
            return false;
        }
        if(started) return true;
        backup::log_memory("before shared background worker creation");
        Result rc = threadCreate(&thread, entry, this, nullptr, 0x20000, 0x2C, -2);
        const char* stage = "create";
        if(R_SUCCEEDED(rc)) {
            created = true;
            stage = "start";
            rc = threadStart(&thread);
            started = R_SUCCEEDED(rc);
            if(!started) {
                if(R_SUCCEEDED(threadClose(&thread))) created = false;
                else stopping = true;
            }
        }
        if(R_FAILED(rc)) {
            error = "Could not " + std::string(stage) + " shared background worker (Result " + std::to_string(rc) + ").";
            backup::log_memory("after failed shared worker start");
            return false;
        }
        backup::log_memory("after shared background worker creation");
        return true;
    }

    bool submit(Job& job, void (*callback)(void*), void* context, std::string& error) {
        std::lock_guard lock(mutex);
        if(!start_locked(error)) return false;
        job.callback = callback;
        job.context = context;
        job.next = nullptr;
        job.complete.store(false);
        job.submitted = true;
        if(tail) tail->next = &job;
        else head = &job;
        tail = &job;
        wake.notify_all();
        return true;
    }

    void wait(Job& job) {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return job.complete.load(); });
    }

    void run() {
        std::unique_lock lock(mutex);
        for(;;) {
            wake.wait(lock, [&] { return stopping || head; });
            if(!head) break;
            Job* job = head;
            head = job->next;
            if(!head) tail = nullptr;
            job->next = nullptr;
            lock.unlock();
            job->callback(job->context);
            lock.lock();
            // Publish completion only after the callback has fully returned.
            job->complete.store(true);
            wake.notify_all();
        }
    }

    bool shutdown(std::string& error) {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            wake.notify_all();
        }
        Result rc = 0;
        if(started) rc = threadWaitForExit(&thread);
        if(R_SUCCEEDED(rc) && created) rc = threadClose(&thread);
        std::lock_guard lock(mutex);
        if(R_FAILED(rc)) {
            error = "Could not release shared background worker (Result " + std::to_string(rc) + ").";
            return false;
        }
        const bool released = created;
        created = started = stopping = false;
        if(released) backup::log_memory("after shared background worker release");
        return true;
    }
};

namespace {
Worker& shared_worker() {
    static Worker worker;
    return worker;
}
} // namespace

Job::~Job() { wait(); }

bool Job::start(void (*callback)(void*), void* context, std::string& error) {
    error.clear();
    if(!callback || !finished()) {
        error = "This background job is already running or has no callback.";
        return false;
    }
    wait();
    return shared_worker().submit(*this, callback, context, error);
}

void Job::wait() {
    if(!submitted) return;
    shared_worker().wait(*this);
    submitted = false;
}

bool Job::reap() {
    if(!submitted || !finished()) return false;
    wait();
    return true;
}

bool shutdown(std::string& error) {
    error.clear();
    return shared_worker().shutdown(error);
}

} // namespace background
