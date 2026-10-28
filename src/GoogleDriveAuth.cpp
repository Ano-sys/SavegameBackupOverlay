#include "include/GoogleDriveAuth.hpp"
#include "include/DriveStorage.hpp"
#include "include/Diagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace drive {
namespace {
using Clock = std::chrono::steady_clock;

HttpReply post(CurlSession& network, const char* url, const std::string& body, std::atomic<bool>& cancelled, long timeout = 15) {
    HttpRequest request;
    request.method = "POST";
    request.url = url;
    request.headers = {"Content-Type: application/x-www-form-urlencoded"};
    request.body = body;
    request.operation = "Google sign-in";
    request.timeout_seconds = timeout;
    request.connect_timeout_seconds = std::min(8L, timeout);
    request.response_limit = 32768;
    return network.request(request, {&cancelled, {}});
}

std::string describe_error(const HttpReply& reply) {
    if(!reply.transport_ok) return backup::diagnostic_text("Google sign-in failed: " + reply.error);

    const std::string error = oauth_error(reply.body);

    if(error == "invalid_client" || error == "unauthorized_client") return "Invalid OAuth client. Use 'TVs and Limited Input devices'.";
    if(error == "access_denied") return "Google sign-in was denied.";
    if(error == "expired_token" || error == "invalid_grant") return "Sign-in code expired. Select Connect to try again.";
    if(error == "invalid_scope") return "Check the Google Drive API and drive.file permission.";

    return "Google sign-in failed (HTTP " + std::to_string(reply.status) + ").";
}

bool wait_until(Clock::time_point time, const std::atomic<bool>& cancelled) {
    while(!cancelled.load() && Clock::now() < time) svcSleepThread(100'000'000);
    return !cancelled.load();
}
} // namespace

GoogleDriveAuth::GoogleDriveAuth(std::string connection_key) : key(std::move(connection_key)) { refresh_connection(); }

GoogleDriveAuth::~GoogleDriveAuth() {
    cancelled.store(true);

    worker.wait();
}

GoogleDriveAuth::View GoogleDriveAuth::view() const {
    std::lock_guard lock(mutex);
    return current;
}

void GoogleDriveAuth::refresh_connection() {
    std::optional<Tokens> tokens;
    std::string resolved_key, error;

    const bool ok = config.effective_connection(key, tokens, resolved_key, error);
    std::lock_guard lock(mutex);

    current.connected = ok && tokens.has_value() && resolved_key == key;
    current.uses_global = ok && tokens.has_value() && resolved_key != key;

    if(!ok) current.message = error;
    else if(current.connected) current.message = "Google Drive is connected.";
    else if(current.uses_global) current.message = "No user connection. Using global Drive.";
    else current.message = "Not connected. Select Connect to sign in to Google.";
}

void GoogleDriveAuth::connect() {
    if(!view().busy) start(true);
}

void GoogleDriveAuth::disconnect() {
    {
        std::lock_guard lock(mutex);

        if(current.busy) {
            disconnect_requested = true;
            cancelled.store(true);
            current.user_code.clear();
            current.qr_url.clear();
            current.verification_url.clear();
            current.message = "Cancelling sign-in ...";
            return;
        }
    }

    start(false);
}

void GoogleDriveAuth::start(bool connect_requested) {
    if(view().busy) return;
    worker.wait();

    cancelled.store(false);

    connecting = connect_requested;

    {
        std::lock_guard lock(mutex);
        disconnect_requested = !connect_requested;
        current.busy = true;
        current.user_code.clear();
        current.verification_url.clear();
        current.qr_url.clear();
        current.qr_includes_code = false;
        current.message = connect_requested ? "Starting Google sign-in ..." : "Disconnecting ...";
    }

    std::string error;
    if(!worker.start(thread_entry, this, error)) {
        std::lock_guard lock(mutex);
        current.busy = false;
        current.message = error;
    }
}

void GoogleDriveAuth::thread_entry(void* context) { static_cast<GoogleDriveAuth*>(context)->run(); }

void GoogleDriveAuth::run() {
    std::string message = connecting ? authorize() : "";
    refresh_connection();

    for(;;) {
        {
            std::lock_guard lock(mutex);

            if(!disconnect_requested) {
                current.busy = false;
                current.user_code.clear();
                current.verification_url.clear();
                current.qr_url.clear();

                if(!message.empty()) current.message = message;

                return;
            }

            disconnect_requested = false;
        }

        std::string error;
        const bool removed = config.remove_connection(key, error);
        refresh_connection();

        // Only remove this local entry. Google revocation could invalidate other
        // connections for the same account and OAuth project as well.
        message = removed ? "Disconnected." : error;

        if(removed && view().uses_global) message = "User connection removed. Using global Drive.";
    }
}

std::string GoogleDriveAuth::authorize() {
    Credentials credentials;
    std::string error;

    if(!config.credentials(credentials, error)) return error;
    if(cancelled.load()) return "Sign-in cancelled.";

    CurlSession network;

    const std::string client = "client_id=" + form_encode(credentials.client_id);
    const auto requested_at = Clock::now();

    HttpReply reply = post(network, "https://oauth2.googleapis.com/device/code", client + "&scope=" + form_encode(Scope), cancelled);

    if(cancelled.load()) return "Sign-in cancelled.";
    if(!reply.success()) return describe_error(reply);

    DeviceAuthorization device;

    if(!parse_device_authorization(reply.body, device, error)) return error;

    const auto expires = requested_at + std::chrono::seconds(device.expires_in);
    {
        std::lock_guard lock(mutex);

        if(cancelled.load()) return "Sign-in cancelled.";

        current.user_code = device.user_code;
        current.verification_url = device.verification_url;
        current.qr_url = device.qr_url();
        current.qr_includes_code = !device.verification_uri_complete.empty();
        current.message = "Scan the QR code on your phone and sign in to Google.";
    }

    const std::string token_request = client + "&client_secret=" + form_encode(credentials.client_secret) + "&device_code=" + form_encode(device.device_code) + "&grant_type=" + form_encode("urn:ietf:params:oauth:grant-type:device_code");
    int interval = device.interval;

    while(Clock::now() < expires) {
        if(!wait_until(std::min(expires, Clock::now() + std::chrono::seconds(interval)), cancelled)) return "Sign-in cancelled.";

        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(expires - Clock::now()).count();

        if(remaining <= 0) break;

        reply = post(network, "https://oauth2.googleapis.com/token", token_request, cancelled, std::min<int64_t>(15, remaining));

        if(cancelled.load()) return "Sign-in cancelled.";
        if(Clock::now() >= expires) break;
        if(reply.success()) {
            Tokens tokens;

            if(!parse_tokens(reply.body, tokens, error)) return error;
            if(cancelled.load()) return "Sign-in cancelled.";
            if(!config.save_connection(key, tokens, error)) return error;

            return "Google Drive connected.";
        }

        const std::string reason = oauth_error(reply.body);

        if(reason == "authorization_pending") continue;
        if(reason == "slow_down") {
            interval = std::min(interval + 5, 86400);
            continue;
        }

        if(!reply.transport_ok || reply.status == 429 || reply.status >= 500) {
            interval = std::min(std::max(interval, 5) * 2, 86400);
            continue;
        }

        return describe_error(reply);
    }

    return "Sign-in code expired. Select Connect to try again.";
}
} // namespace drive
