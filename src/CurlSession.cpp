#include "include/DriveStorage.hpp"
#include "include/Diagnostics.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <condition_variable>
#include <mutex>
#ifndef __SWITCH__
#include <thread>
#endif
#include <sys/socket.h>

namespace drive {
namespace {
using File = std::unique_ptr<FILE, int (*)(FILE*)>;

std::string file_error(const char* operation, int code) {
    if(!code) code = EIO;
    return std::string(operation) + ": " + std::strerror(code) + " (errno " + std::to_string(code) + ").";
}

bool socket_error(CURLcode result) {
    switch(result) {
        case CURLE_COULDNT_CONNECT:
        case CURLE_FAILED_INIT:
        case CURLE_INTERFACE_FAILED:
        case CURLE_OPERATION_TIMEDOUT:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
            return true;
        default:
            return false;
    }
}

struct SocketDiagnostics {
    bool failed = false;
    int error = 0;
#ifdef __SWITCH__
    Result result = 0;
#endif
};

struct ResetRequestOptions {
    CURL* handle;
    ~ResetRequestOptions() { curl_easy_reset(handle); }
};

#ifdef __SWITCH__
std::string result_code(Result result) {
    char code[11]{};
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(result));
    return code;
}
#endif

curl_socket_t open_socket(void* context, curlsocktype, curl_sockaddr* address) {
    auto& diagnostic = *static_cast<SocketDiagnostics*>(context);
    errno = 0;
    const auto socket = ::socket(address->family, address->socktype, address->protocol);

    if(socket == CURL_SOCKET_BAD) {
        diagnostic.failed = true;
        diagnostic.error = errno;
#ifdef __SWITCH__
        diagnostic.result = socketGetLastResult();
#endif
        errno = diagnostic.error;
    }

    return socket;
}

struct Transfer {
    const HttpRequest& request;
    const backup::Control& control;
    HttpReply& reply;
    FILE* output;
    uint64_t received = 0;
    FILE* input = nullptr;
};

size_t send(char* data, size_t size, size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    if(transfer.control.stopped()) return CURL_READFUNC_ABORT;
    errno = 0;
    const auto read = std::fread(data, 1, size * count, transfer.input);
    if(std::ferror(transfer.input)) {
        transfer.reply.error = file_error("Read backup ZIP", errno);
        return CURL_READFUNC_ABORT;
    }
    return read;
}

size_t receive(char* data, size_t size, size_t count, void* context) {
    auto& transfer = *static_cast<Transfer*>(context);
    const size_t bytes = size * count;

    if(transfer.control.stopped()) return 0;

    const bool download = transfer.output && transfer.reply.status >= 200 && transfer.reply.status < 300;
    const uint64_t limit = download ? transfer.request.transfer_size : transfer.request.response_limit;

    if(bytes > limit || transfer.received > limit - bytes) {
        transfer.reply.error = "Response exceeds the allowed size (" + std::to_string(limit) + " bytes).";
        return 0;
    }

    transfer.received += bytes;

    if(download) {
        errno = 0;
        const auto written = std::fwrite(data, 1, bytes, transfer.output);
        if(written != bytes) transfer.reply.error = file_error("Write downloaded ZIP", errno);
        return written;
    }

    transfer.reply.body.append(data, bytes);
    return bytes;
}

size_t header(char* data, size_t size, size_t count, void* context) {
    auto& reply = *static_cast<HttpReply*>(context);
    const size_t bytes = size * count;

    if(bytes > 8192) {
        reply.error = "HTTP response header exceeds 8192 bytes.";
        return 0;
    }

    std::string line(data, bytes);

    if(line.starts_with("HTTP/")) {
        std::sscanf(line.c_str(), "HTTP/%*s %ld", &reply.status);
        return bytes;
    }

    const size_t colon = line.find(':');

    if(colon == std::string::npos) return bytes;

    std::string name = line.substr(0, colon);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if(name == "location") {
        const auto begin = line.find_first_not_of(" \t", colon + 1);
        const auto end = line.find_last_not_of("\r\n \t");

        if(begin != std::string::npos && end >= begin) reply.location = line.substr(begin, end - begin + 1);
    }

    return bytes;
}

int progress(void* context, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    auto& transfer = *static_cast<Transfer*>(context);

    if(transfer.control.stopped()) return 1;
    if(!transfer.request.upload_path.empty()) transfer.control.report("Uploading complete save ...", std::max<curl_off_t>(0, uploaded), std::max<curl_off_t>(0, upload_total));
    else if(transfer.output) transfer.control.report("Downloading backup ...", std::max<curl_off_t>(0, downloaded), std::max<curl_off_t>(0, download_total));
    return 0;
}

// The lightweight CurlSession objects all use this one process-wide transport.
// The mutex also protects curl callbacks and request headers from other workers.
struct SharedNetwork {
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::condition_variable wake;
    CURL* handle = nullptr;
    bool sockets = false;
    bool curl = false;
    bool stopping = false;
    Clock::time_point last_activity{};
#ifdef __SWITCH__
    Thread monitor{};
    bool monitor_created = false;
#else
    std::thread monitor;
#endif

    ~SharedNetwork() { shutdown(); }

    void close_locked() {
        if(!handle && !curl && !sockets) return;
        if(handle) curl_easy_cleanup(handle);
        handle = nullptr;
        if(curl) curl_global_cleanup();
        curl = false;
#ifdef __SWITCH__
        if(sockets) socketExit();
#endif
        sockets = false;
        backup::log_memory("after shared network cleanup");
    }

    bool expire_locked(Clock::time_point now) {
        if(!handle || now - last_activity < CurlSession::IdleTimeout) return false;
        close_locked();
        return true;
    }

    void maintain() {
        std::unique_lock lock(mutex);
        while(!stopping) {
            if(!handle) wake.wait(lock, [&] { return stopping || handle; });
            else {
                wake.wait_until(lock, last_activity + CurlSession::IdleTimeout);
                if(!stopping) expire_locked(Clock::now());
            }
        }
    }

    bool start_monitor(std::string& error) {
#ifdef __SWITCH__
        if(monitor_created) return true;
        Result rc = threadCreate(&monitor, [](void* context) { static_cast<SharedNetwork*>(context)->maintain(); }, this, nullptr, 0x8000, 0x2C, -2);
        if(R_SUCCEEDED(rc)) {
            monitor_created = true;
            rc = threadStart(&monitor);
            if(R_FAILED(rc)) {
                if(R_SUCCEEDED(threadClose(&monitor))) monitor_created = false;
                else stopping = true;
            }
        }
        if(R_FAILED(rc)) {
            error = "Could not start the network idle timer (Result " + std::to_string(rc) + ").";
            return false;
        }
#else
        (void)error;
        if(!monitor.joinable()) monitor = std::thread([this] { maintain(); });
#endif
        return true;
    }

    bool initialize(std::string& error) {
        // Also enforce expiry if the overlay was suspended while idle.
        expire_locked(Clock::now());
        if(handle) return true;
        backup::log_memory("before shared socket initialization");
#ifdef __SWITCH__
        const SocketInitConfig settings = {
            .tcp_tx_buf_size = 0x4000,
            .tcp_rx_buf_size = 0x4000,
            .tcp_tx_buf_max_size = 0,
            .tcp_rx_buf_max_size = 0,
            .udp_tx_buf_size = 0x1000,
            .udp_rx_buf_size = 0x1000,
            // Keep this contiguous 160 KiB block across operations.
            .sb_efficiency = 4,
            .num_bsd_sessions = 1,
            .bsd_service_type = BsdServiceType_Auto,
        };
        const Result rc = socketInitialize(&settings);
        sockets = R_SUCCEEDED(rc);
        if(!sockets) {
            const auto reason = rc == MAKERESULT(Module_Libnx, LibnxError_OutOfMemory) ? "out of memory" : "service error";
            error = "Socket initialization failed: " + std::string(reason) + " (Result " + std::to_string(rc) + ", " + result_code(rc) + ").";
            backup::log_memory("after failed shared socket initialization");
            return false;
        }
#endif
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = result == CURLE_OK;
        if(!curl) error = "curl_global_init failed (curl " + std::to_string(result) + "): " + curl_easy_strerror(result);
        else {
            handle = curl_easy_init();
            if(!handle) error = "curl_easy_init failed. No request was sent.";
        }
        if(!handle || !start_monitor(error)) {
            close_locked();
            return false;
        }
        backup::log_memory("after shared network initialization");
        return true;
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            wake.notify_all();
        }
        bool stopped = true;
#ifdef __SWITCH__
        if(monitor_created) {
            Result rc = threadWaitForExit(&monitor);
            if(R_SUCCEEDED(rc)) rc = threadClose(&monitor);
            stopped = R_SUCCEEDED(rc);
            if(stopped) monitor_created = false;
            else backup::log_error("Could not release the network idle timer (Result " + std::to_string(rc) + ").");
        }
#else
        if(monitor.joinable()) monitor.join();
#endif
        std::lock_guard lock(mutex);
        close_locked();
        stopping = !stopped;
    }
};

SharedNetwork& shared_network() {
    static SharedNetwork network;
    return network;
}

struct TouchNetwork {
    SharedNetwork& network;
    ~TouchNetwork() {
        network.last_activity = SharedNetwork::Clock::now();
        network.wake.notify_all();
    }
};
} // namespace

bool CurlSession::expire_idle(std::chrono::steady_clock::time_point now) {
    auto& shared = shared_network();
    std::unique_lock lock(shared.mutex, std::try_to_lock);
    return lock && !shared.stopping && shared.expire_locked(now);
}

void CurlSession::shutdown() { shared_network().shutdown(); }

HttpReply CurlSession::request(const HttpRequest& request, const backup::Control& control) const {
    HttpReply reply;

    if(control.stopped()) {
        reply.error = "Operation cancelled.";
        return reply;
    }

    // Request options are reset before these diagnostics leave scope.
    SocketDiagnostics socket_diagnostic;
    char error_buffer[CURL_ERROR_SIZE]{};

    File input(nullptr, std::fclose), output(nullptr, std::fclose);

    if(!request.upload_path.empty()) {
        input.reset(std::fopen(request.upload_path.c_str(), "rb"));

        if(!input) {
            reply.error = file_error("Open backup ZIP", errno);
            return reply;
        }
    }

    if(!request.download_path.empty()) {
        output.reset(std::fopen(request.download_path.c_str(), "wb"));

        if(!output) {
            reply.error = file_error("Open download file", errno);
            return reply;
        }
    }

    auto& shared = shared_network();
    std::unique_lock lock(shared.mutex, std::defer_lock);
    while(!lock.try_lock()) {
        if(control.stopped()) {
            reply.error = "Operation cancelled.";
            return reply;
        }
#ifdef __SWITCH__
        svcSleepThread(10'000'000);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
    }
    if(control.stopped() || shared.stopping) {
        reply.error = "Operation cancelled.";
        return reply;
    }
    if(!shared.initialize(reply.error)) return reply;
    const TouchNetwork touch{shared};
    CURL* handle = shared.handle;

    Transfer transfer{request, control, reply, output.get()};
    transfer.input = input.get();
    // Clear callbacks and request data while retaining the connection cache.
    const ResetRequestOptions reset_options{handle};
    curl_slist* headers = nullptr;

    for(const auto& value : request.headers) {
        auto* appended = curl_slist_append(headers, value.c_str());

        if(!appended) {
            reply.error = "Could not allocate HTTP request headers.";
            curl_slist_free_all(headers);
            return reply;
        }

        headers = appended;
    }

    const auto set_option = [&](CURLoption option, auto value) {
        const auto result = curl_easy_setopt(handle, option, value);
        if(result != CURLE_OK && reply.error.empty()) reply.error = "Set curl option " + std::to_string(option) + " failed (curl " + std::to_string(result) + "): " + curl_easy_strerror(result);
    };
    set_option(CURLOPT_ERRORBUFFER, error_buffer);
    set_option(CURLOPT_URL, request.url.c_str());
    set_option(CURLOPT_OPENSOCKETFUNCTION, open_socket);
    set_option(CURLOPT_OPENSOCKETDATA, &socket_diagnostic);
    set_option(CURLOPT_HTTPHEADER, headers);
    set_option(CURLOPT_MAXCONNECTS, 2L);
#if LIBCURL_VERSION_NUM >= 0x074100
    set_option(CURLOPT_MAXAGE_CONN, 300L);
#endif
    set_option(CURLOPT_CONNECTTIMEOUT, request.connect_timeout_seconds);
    set_option(CURLOPT_TIMEOUT, request.timeout_seconds > 0 ? request.timeout_seconds : input || output ? 1800L
                                                                                                        : 30L);
    set_option(CURLOPT_LOW_SPEED_LIMIT, 1L);
    set_option(CURLOPT_LOW_SPEED_TIME, 30L);
    set_option(CURLOPT_NOSIGNAL, 1L);
    set_option(CURLOPT_SSL_VERIFYPEER, 1L);
    set_option(CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
    set_option(CURLOPT_PROTOCOLS_STR, "https");
#else
    set_option(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif
    set_option(CURLOPT_WRITEFUNCTION, receive);
    set_option(CURLOPT_WRITEDATA, &transfer);
    set_option(CURLOPT_HEADERFUNCTION, header);
    set_option(CURLOPT_HEADERDATA, &reply);
    set_option(CURLOPT_NOPROGRESS, 0L);
    set_option(CURLOPT_XFERINFOFUNCTION, progress);
    set_option(CURLOPT_XFERINFODATA, &transfer);

    if(input) {
        set_option(CURLOPT_UPLOAD, 1L);
        set_option(CURLOPT_READFUNCTION, send);
        set_option(CURLOPT_READDATA, &transfer);
        set_option(CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.transfer_size));
    }
    else if(request.method == "POST" || request.method == "PATCH") {
        set_option(CURLOPT_POSTFIELDS, request.body.c_str());
        set_option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
    }

    if(request.method != "GET") set_option(CURLOPT_CUSTOMREQUEST, request.method.c_str());

    if(!reply.error.empty()) {
        curl_slist_free_all(headers);
        return reply;
    }

    const auto result = curl_easy_perform(handle);
#ifdef __SWITCH__
    // Snapshot the last BSD result before cleanup or another socket operation.
    const Result socket_result = socketGetLastResult();
#endif
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &reply.status);
    reply.transport_ok = result == CURLE_OK;
    if(result != CURLE_OK) {
        const auto detail = error_buffer[0] ? error_buffer : curl_easy_strerror(result);
        std::string cause = "curl " + std::to_string(result) + ": " + detail;

        if(socket_error(result)) {
            long socket_errno = 0;
            const auto info_result = curl_easy_getinfo(handle, CURLINFO_OS_ERRNO, &socket_errno);

            if(info_result != CURLE_OK) cause += " [socket errno unavailable: " + std::string(curl_easy_strerror(info_result)) + "]";
            else if(socket_errno > 0) cause += " [socket errno " + std::to_string(socket_errno) + ": " + std::strerror(static_cast<int>(socket_errno)) + "]";
            else if(!socket_diagnostic.failed) cause += " [socket errno not reported]";

            if(socket_diagnostic.failed) {
                // curl 7.69.1 loses errno when socket creation itself fails.
                cause += socket_diagnostic.error > 0 ? " [last socket() failure: errno " + std::to_string(socket_diagnostic.error) + ": " + std::strerror(socket_diagnostic.error) + "]" : " [socket() failed without reporting errno]";
#ifdef __SWITCH__
                cause += " [socket() BSD Result " + result_code(socket_diagnostic.result) + "]";
#endif
            }

#ifdef __SWITCH__
            if(!socket_diagnostic.failed) cause += " [last BSD Result " + result_code(socket_result) + "]";
#endif
        }

        reply.error = reply.error.empty() ? cause : reply.error + " " + cause;
    }
    curl_slist_free_all(headers);

    if(output) {
        if(std::fflush(output.get()) != 0) {
            reply.transport_ok = false;
            reply.error = file_error("Flush downloaded ZIP", errno);
        }

        FILE* file = output.release();

        if(std::fclose(file) != 0) {
            reply.transport_ok = false;
            reply.error = file_error("Close downloaded ZIP", errno);
        }
    }

    return reply;
}

} // namespace drive
