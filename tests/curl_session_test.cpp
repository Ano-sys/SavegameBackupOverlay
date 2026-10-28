#include "include/DriveStorage.hpp"
#include "include/Diagnostics.hpp"

#include <curl/curl.h>
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sys/socket.h>

namespace {
bool simulate_connect_failure = false;
long simulated_socket_errno = ECONNREFUSED;
CURLcode simulated_info_result = CURLE_OK;
bool simulate_socket_failure = false;
int simulated_open_errno = EMFILE;
unsigned handles_created = 0;
int request_marker = 0;
std::mutex request_mutex;
std::condition_variable request_wake;
bool hold_request = false, request_entered = false, release_request = false;
unsigned active_requests = 0, peak_requests = 0;
} // namespace

extern "C" CURL* __real_curl_easy_init();
extern "C" CURLcode __real_curl_easy_perform(CURL* handle);
extern "C" CURLcode __real_curl_easy_getinfo(CURL* handle, CURLINFO info, ...);
extern "C" int __real_socket(int domain, int type, int protocol);

extern "C" CURL* __wrap_curl_easy_init() {
    ++handles_created;
    return __real_curl_easy_init();
}

extern "C" int __wrap_socket(int domain, int type, int protocol) {
    if(!simulate_socket_failure) return __real_socket(domain, type, protocol);
    errno = simulated_open_errno;
    return -1;
}

extern "C" CURLcode __wrap_curl_easy_perform(CURL* handle) {
    // Previous request options must not survive on a reused handle.
    char* previous_marker = nullptr;
    assert(__real_curl_easy_getinfo(handle, CURLINFO_PRIVATE, &previous_marker) == CURLE_OK);
    assert(previous_marker == nullptr);
    assert(curl_easy_setopt(handle, CURLOPT_PRIVATE, &request_marker) == CURLE_OK);
    {
        std::unique_lock lock(request_mutex);
        if(hold_request) {
            ++active_requests;
            peak_requests = std::max(peak_requests, active_requests);
            request_entered = true;
            request_wake.notify_all();
            request_wake.wait(lock, [] { return release_request; });
            --active_requests;
            return CURLE_OK;
        }
    }
    return simulate_connect_failure ? CURLE_COULDNT_CONNECT : __real_curl_easy_perform(handle);
}

extern "C" CURLcode __wrap_curl_easy_getinfo(CURL* handle, CURLINFO info, ...) {
    // CurlSession currently requests only long-valued information.
    assert(info == CURLINFO_RESPONSE_CODE || info == CURLINFO_OS_ERRNO);
    va_list args;
    va_start(args, info);
    auto* value = va_arg(args, long*);
    va_end(args);

    if(!simulate_connect_failure) return __real_curl_easy_getinfo(handle, info, value);
    *value = info == CURLINFO_OS_ERRNO ? simulated_socket_errno : 0;
    return info == CURLINFO_OS_ERRNO ? simulated_info_result : CURLE_OK;
}

int main() {
    namespace fs = std::filesystem;
    char directory[] = "/tmp/sbo-curl-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory);

    drive::CurlSession session;
    assert(handles_created == 0);
    drive::HttpRequest request;
    request.url = "https://www.googleapis.com/upload/?upload_id=private-session";
    request.upload_path = (root / "missing.zip").string();
    auto reply = session.request(request, {});
    assert(!reply.transport_ok);
    assert(reply.error.find("Open backup ZIP") != std::string::npos);
    assert(reply.error.find("errno 2") != std::string::npos);
    assert(reply.error.find("private-session") == std::string::npos);

    request.upload_path.clear();
    request.download_path = (root / "missing/destination.zip").string();
    reply = session.request(request, {});
    assert(!reply.transport_ok && reply.error.find("Open download file") != std::string::npos);
    assert(handles_created == 0);

    // The rejected protocol produces a real curl error without network access.
    request.download_path.clear();
    request.url = "file:///unused";
    reply = session.request(request, {});
    assert(!reply.transport_ok && reply.error.find("curl 1:") != std::string::npos);
    assert(reply.error.find("socket errno") == std::string::npos);

    // Simulate refused connections without accessing a real network.
    request.url = "https://www.googleapis.com/drive/v3/files?fields=id";
    simulate_connect_failure = true;
    reply = session.request(request, {});
    assert(!reply.transport_ok && reply.status == 0);
    assert(reply.error.find("curl 7:") != std::string::npos);
    assert(reply.error.find("socket errno " + std::to_string(ECONNREFUSED) + ": " + std::strerror(ECONNREFUSED)) != std::string::npos);

    simulated_socket_errno = 0;
    reply = session.request(request, {});
    assert(reply.error.find("socket errno not reported") != std::string::npos);

    simulated_info_result = CURLE_UNKNOWN_OPTION;
    reply = session.request(request, {});
    assert(reply.error.find("socket errno unavailable:") != std::string::npos);
    simulate_connect_failure = false;

    // Exercise real curl's socket callback with no DNS or network connection.
    assert(setenv("no_proxy", "*", 1) == 0);
    request.url = "https://127.0.0.1:443/";
    simulate_socket_failure = true;
    for(int code : {EMFILE, ENOBUFS}) {
        simulated_open_errno = code;
        reply = session.request(request, {});
        assert(!reply.transport_ok && reply.error.find("curl 7:") != std::string::npos);
        assert(reply.error.find("last socket() failure: errno " + std::to_string(code) + ": " + std::strerror(code)) != std::string::npos);
        assert(reply.error.find("socket errno not reported") == std::string::npos);
    }
    simulated_open_errno = 0;
    reply = session.request(request, {});
    assert(reply.error.find("socket() failed without reporting errno") != std::string::npos);
    simulate_socket_failure = false;
    assert(handles_created == 1);

    // New operation objects must retain the same shared easy handle/cache.
    {
        drive::CurlSession next_operation;
        request.url = "file:///unused";
        reply = next_operation.request(request, {});
        assert(!reply.transport_ok && handles_created == 1);
    }
    assert(!drive::CurlSession::expire_idle());

    const auto future = std::chrono::steady_clock::now() + drive::CurlSession::IdleTimeout + std::chrono::seconds(1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    bool expired = false;
    while(!(expired = drive::CurlSession::expire_idle(future)) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(expired);

    assert(!drive::CurlSession::expire_idle());
    reply = session.request(request, {});
    assert(!reply.transport_ok && handles_created == 2);

    // Expiry must not close an active request. Other operation objects wait
    // for that request, and cancellation can interrupt the wait.
    request.url = "https://example.test/serialized";
    hold_request = true;
    std::thread first([&] { session.request(request, {}); });
    {
        std::unique_lock lock(request_mutex);
        assert(request_wake.wait_for(lock, std::chrono::seconds(5), [] { return request_entered; }));
    }
    assert(!drive::CurlSession::expire_idle(std::chrono::steady_clock::now() + std::chrono::hours(1)));
    std::atomic<bool> queued_cancel{false};
    std::thread queued([&] {
        drive::CurlSession next_operation;
        const auto cancelled = next_operation.request(request, {&queued_cancel, {}});
        assert(!cancelled.transport_ok && cancelled.error == "Operation cancelled.");
    });
    queued_cancel.store(true);
    queued.join();
    std::thread second([&] {
        drive::CurlSession next_operation;
        next_operation.request(request, {});
    });
    {
        std::lock_guard lock(request_mutex);
        release_request = true;
        request_wake.notify_all();
    }
    first.join();
    second.join();
    assert(peak_requests == 1 && handles_created == 2);
    hold_request = false;

    std::atomic<bool> cancelled{true};
    reply = session.request(request, {&cancelled, {}});
    assert(!reply.transport_ok && reply.error == "Operation cancelled.");

    const auto safe = backup::diagnostic_text(
        "curl 60: certificate error\nBearer private-access https://example.test/private-session?upload_id=secret private-refresh",
        {"private-access", "private-refresh"}
    );
    assert(safe.find("curl 60") != std::string::npos);
    assert(safe.find("private-access") == std::string::npos);
    assert(safe.find("private-refresh") == std::string::npos);
    assert(safe.find("private-session") == std::string::npos);
    assert(safe.find("https://") == std::string::npos && safe.find('\n') == std::string::npos);
    assert(backup::diagnostic_text(std::string(5000, 'x')).size() == 2051);

    const auto filesystem = backup::filesystem_error("Write restore destination", "cloudsave:/slot", EIO, 0x0000c802);
    assert(filesystem.find("cloudsave:/slot") != std::string::npos);
    assert(filesystem.find("errno " + std::to_string(EIO)) != std::string::npos);
    assert(filesystem.find(std::strerror(EIO)) != std::string::npos);
    assert(filesystem.find("last FS Result 0x0000C802, module 2, description 100") != std::string::npos);
    const auto no_native = backup::filesystem_error("Open file", "/missing", ENOENT);
    assert(no_native.find("FS Result") == std::string::npos);
    const auto no_errno = backup::filesystem_error("Commit save", "cloudsave:/", 0, 1);
    assert(no_errno.find("no errno reported") != std::string::npos);

    const auto previous = fs::current_path();
    fs::current_path(root);
    fs::create_directories(backup::DataDirectory);
    assert(backup::log_error(safe));
    const auto path = fs::path(backup::DataDirectory) / "error.log";
    {
        std::ifstream file(path);
        const std::string log{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        assert(log.find(safe) != std::string::npos && log.find("private-session") == std::string::npos);
    }
    {
        std::ofstream file(path);
        file << std::string(64 * 1024, 'x');
    }
    assert(backup::log_error("new error"));
    assert(fs::file_size(path) < 1024);
    fs::current_path(previous);
    fs::remove_all(root);
    drive::CurlSession::shutdown();
    std::puts("Lazy shared Curl handle, cross-operation reuse, idle expiry/recreation, request isolation, socket diagnostics, and logging passed.");
}
