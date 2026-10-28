#include "include/BackgroundJob.hpp"

#include <switch.h>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace {
struct Call {
    unsigned id;
    bool block = false;
};
std::mutex mutex;
std::condition_variable wake;
bool entered = false, release = false;
std::vector<unsigned> order;

void perform(void* context) {
    const auto& call = *static_cast<Call*>(context);
    std::unique_lock lock(mutex);
    if(call.block) {
        entered = true;
        wake.notify_all();
        wake.wait(lock, [] { return release; });
    }
    order.push_back(call.id);
}
} // namespace

int main() {
    std::string error;
    Call first{1, true}, second{2}, third{3};
    background::Job a, b, c;
    assert(test_thread_creations == 0);
    test_thread_create_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    assert(!a.start(perform, &first, error));
    assert(error.find("create shared background worker (Result 1369)") != std::string::npos);
    assert(a.finished() && test_open_threads == 0);
    test_thread_create_result = 0;
    test_thread_start_result = 77;
    assert(!a.start(perform, &first, error));
    assert(error.find("start shared background worker (Result 77)") != std::string::npos);
    assert(a.finished() && test_open_threads == 0);
    test_thread_start_result = 0;

    assert(a.start(perform, &first, error));
    {
        std::unique_lock lock(mutex);
        assert(wake.wait_for(lock, std::chrono::seconds(5), [] { return entered; }));
    }
    assert(!a.start(perform, &first, error));
    const auto creations = test_thread_creations;
    test_thread_create_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    assert(b.start(perform, &second, error) && c.start(perform, &third, error));
    assert(test_thread_creations == creations && test_open_threads == 1);
    {
        std::lock_guard lock(mutex);
        release = true;
        wake.notify_all();
    }
    a.wait();
    b.wait();
    c.wait();
    assert((order == std::vector<unsigned>{1, 2, 3}));
    assert(test_open_threads == 1);
    assert(a.start(perform, &second, error));
    a.wait();
    assert(test_thread_creations == creations && order.back() == 2);

    // A failed release must retain the original handle and prevent replacing it.
    test_thread_close_result = 77;
    assert(!background::shutdown(error) && error.find("Result 77") != std::string::npos);
    assert(test_open_threads == 1);
    assert(!a.start(perform, &second, error) && test_thread_creations == creations);
    test_thread_close_result = 0;
    assert(background::shutdown(error) && test_open_threads == 0);
    assert(background::shutdown(error) && test_open_threads == 0);
    test_thread_create_result = 0;
    assert(a.start(perform, &third, error));
    a.wait();
    assert(test_thread_creations == creations + 1);
    assert(background::shutdown(error) && test_open_threads == 0);

    std::puts("One shared worker, FIFO jobs, stack-allocation failures, repeated operations, safe completion, and retained failed-close handles passed.");
}
