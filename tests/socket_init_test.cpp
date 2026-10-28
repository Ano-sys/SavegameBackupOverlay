#include "include/DriveStorage.hpp"

#include <switch.h>
#include <cassert>
#include <cstdio>

namespace {
Result initialization_result = 0;
unsigned exits = 0;
unsigned initializations = 0;
SocketInitConfig last_settings{};
} // namespace

namespace backup {
void log_memory(const char*) {}
bool log_error(const std::string&) { return true; }
} // namespace backup

Result socketInitialize(const SocketInitConfig* settings) {
    ++initializations;
    last_settings = *settings;
    return initialization_result;
}
Result socketGetLastResult() { return 0; }
void socketExit() { ++exits; }

int main() {
    static_assert(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory) == 1369);
    initialization_result = 1369;
    drive::HttpRequest request;
    request.url = "file:///unused"; // Rejected locally after initialization; no HTTP is sent.
    {
        drive::CurlSession session;
        assert(initializations == 0);
        const auto reply = session.request(request, {});
        assert(!reply.transport_ok);
        assert(reply.error.find("out of memory") != std::string::npos);
        assert(reply.error.find("1369") != std::string::npos);
        assert(reply.error.find("0x00000559") != std::string::npos);
    }
    assert(exits == 0);

    initialization_result = 0;
    {
        drive::CurlSession session;
        session.request(request, {});
        assert(initializations == 2);
        assert(last_settings.sb_efficiency == 4);
    }
    assert(exits == 0 && test_open_threads == 1);
    {
        drive::CurlSession session;
        for(unsigned i = 0; i < 10; ++i) session.request(request, {});
        assert(initializations == 2 && exits == 0);
        assert(!drive::CurlSession::expire_idle());

        const auto future = std::chrono::steady_clock::now() + drive::CurlSession::IdleTimeout + std::chrono::seconds(1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

        bool expired = false;
        while(!(expired = drive::CurlSession::expire_idle(future)) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(expired);

        assert(exits == 1);
        session.request(request, {});
        assert(initializations == 3 && test_open_threads == 1);
    }
    drive::CurlSession::shutdown();
    assert(exits == 2 && test_open_threads == 0);
    drive::CurlSession::shutdown();
    assert(exits == 2);
    std::puts("Switch shared socket ownership, idle expiry, recreation, out-of-memory diagnostics, and final cleanup passed.");
}
