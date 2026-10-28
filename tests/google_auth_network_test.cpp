#include "include/GoogleDriveAuth.hpp"
#include "include/DriveStorage.hpp"

#include <cassert>
#include <filesystem>

namespace {
unsigned requests = 0;
}

namespace drive {
HttpReply CurlSession::request(const HttpRequest& request, const backup::Control& control) const {
    assert(!control.stopped());
    assert(request.method == "POST" && request.headers == std::vector<std::string>{"Content-Type: application/x-www-form-urlencoded"});
    assert(request.timeout_seconds > 0 && request.timeout_seconds <= 15);
    assert(request.connect_timeout_seconds <= 8 && request.response_limit == 32768);
    for(const auto& header : request.headers) assert(!header.starts_with("Authorization:"));
    ++requests;
    if(request.url == "https://oauth2.googleapis.com/device/code") {
        assert(requests == 1 && request.body.find("client_id=") != std::string::npos);
        return {true, 200, R"({"device_code":"test-device","user_code":"1234","verification_url":"https://www.google.com/device","expires_in":600,"interval":1})", ""};
    }
    assert(request.url == "https://oauth2.googleapis.com/token" && requests == 2);
    assert(request.body.find("device_code=test-device") != std::string::npos);
    return {true, 200, std::string(R"({"access_token":"access","refresh_token":"refresh","expires_in":3600,"token_type":"Bearer","scope":")") + Scope + "\"}", ""};
}
} // namespace drive

int main() {
    namespace fs = std::filesystem;
    char directory[] = "/tmp/sbo-auth-network-test-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory), previous = fs::current_path();
    fs::current_path(root);
    fs::create_directory("sdmc:");
    {
        drive::GoogleDriveAuth auth("global");
        auth.connect();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(auth.view().busy && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto result = auth.view();
        assert(!result.busy && result.connected && requests == 2);
        assert(result.message == "Google Drive connected.");
    }
    assert(test_open_threads == 1);
    std::string error;
    assert(background::shutdown(error) && test_open_threads == 0);
    fs::current_path(previous);
    fs::remove_all(root);
    std::puts("Google sign-in uses the common HTTP transport with bounded responses, request timeouts, and clean worker shutdown.");
}
