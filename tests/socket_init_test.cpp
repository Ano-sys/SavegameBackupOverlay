#include "include/DriveStorage.hpp"

#include <switch.h>
#include <cassert>
#include <cstdio>

namespace {
Result initialization_result = 0;
unsigned exits = 0;
SocketInitConfig last_settings{};
} // namespace

namespace backup {
void log_memory(const char*) {}
} // namespace backup

Result socketInitialize(const SocketInitConfig* settings) {
    last_settings = *settings;
    return initialization_result;
}
Result socketGetLastResult() { return 0; }
void socketExit() { ++exits; }

int main() {
    static_assert(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory) == 1369);
    initialization_result = 1369;
    {
        drive::CurlSession session;
        assert(!session.ready());
        assert(session.error().find("out of memory") != std::string::npos);
        assert(session.error().find("1369") != std::string::npos);
        assert(session.error().find("0x00000559") != std::string::npos);
    }
    assert(exits == 0);

    initialization_result = 0;
    {
        drive::CurlSession session;
        assert(session.ready());
        assert(last_settings.sb_efficiency == 4);
    }
    assert(exits == 1);
    std::puts("Switch socket out-of-memory diagnostics and cleanup passed.");
}
