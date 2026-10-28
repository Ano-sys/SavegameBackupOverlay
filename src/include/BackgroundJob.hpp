#pragma once

#include <atomic>
#include <string>

namespace background {

struct Worker;

// Each owner waits for its callback before destroying the callback's context.
// All jobs share a single stack, retained until the overlay closes.
class Job {
public:
    Job() = default;
    ~Job();
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;

    bool start(void (*callback)(void*), void* context, std::string& error);
    bool finished() const { return complete.load(); }
    void wait();
    bool reap();

private:
    friend struct Worker;
    void (*callback)(void*) = nullptr;
    void* context = nullptr;
    Job* next = nullptr;
    std::atomic<bool> complete{true};
    bool submitted = false;
};

// Called after all job owners (GUI tasks) have stopped.
bool shutdown(std::string& error);

} // namespace background
