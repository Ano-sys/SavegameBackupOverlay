#include "include/GoogleDriveAuth.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace drive {
namespace {
using Clock = std::chrono::steady_clock;

struct Network {
    bool sockets = false;
    bool curl = false;

    bool initialize() {
        const SocketInitConfig settings = {
            .tcp_tx_buf_size = 0x4000,
            .tcp_rx_buf_size = 0x4000,
            .tcp_tx_buf_max_size = 0,
            .tcp_rx_buf_max_size = 0,
            .udp_tx_buf_size = 0x1000,
            .udp_rx_buf_size = 0x1000,
            .sb_efficiency = 1,
            .num_bsd_sessions = 1,
            .bsd_service_type = BsdServiceType_Auto,
        };

        sockets = R_SUCCEEDED(socketInitialize(&settings));

        if(sockets) curl = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;

        return sockets && curl;
    }

    ~Network() {
        if(curl) curl_global_cleanup();

        if(sockets) socketExit();
    }
};

struct Reply {
    CURLcode result = CURLE_FAILED_INIT;
    long status = 0;
    std::string body;
    bool success() const { return result == CURLE_OK && status >= 200 && status < 300; }
};

size_t receive(char* data, size_t size, size_t count, void* context) {
    auto& body = *static_cast<std::string*>(context);

    const size_t bytes = size * count;

    if(bytes > 32768 || body.size() > 32768 - bytes) return 0;

    body.append(data, bytes);

    return bytes;
}

int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return static_cast<std::atomic<bool>*>(context)->load() ? 1 : 0; }

Reply post(const char* url, const std::string& body, std::atomic<bool>& cancelled, long timeout = 15) {
    Reply reply;

    if(cancelled.load()) {
        reply.result = CURLE_ABORTED_BY_CALLBACK;
        return reply;
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);

    if(!curl) return reply;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, std::min(8L, timeout));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &reply.body);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancelled);

    // The Switch libcurl backend verifies against the system certificate store.
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);

    reply.result = curl_easy_perform(curl.get());

    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &reply.status);

    return reply;
}

std::string describe_error(const Reply& reply) {
    if(reply.result != CURLE_OK) return "Cannot reach Google. Check Wi-Fi and date/time.";

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

    if(worker_created) {
        threadWaitForExit(&worker);
        threadClose(&worker);
    }
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
    if(worker_created) {
        if(!finished.load()) return;

        threadWaitForExit(&worker);
        threadClose(&worker);
        worker_created = false;
    }

    cancelled.store(false);
    finished.store(false);

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

    Result result = threadCreate(&worker, thread_entry, this, nullptr, 0x18000, 0x2c, -2);

    if(R_SUCCEEDED(result)) {
        worker_created = true;
        result = threadStart(&worker);

        if(R_SUCCEEDED(result)) return;

        threadClose(&worker);
        worker_created = false;
    }

    finished.store(true);

    std::lock_guard lock(mutex);
    current.busy = false;
    current.message = "Could not start sign-in thread.";
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

                finished.store(true);
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

    Network network;

    if(!network.initialize()) return "Could not initialize network.";

    const std::string client = "client_id=" + form_encode(credentials.client_id);
    const auto requested_at = Clock::now();

    Reply reply = post("https://oauth2.googleapis.com/device/code", client + "&scope=" + form_encode(Scope), cancelled);

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

        reply = post("https://oauth2.googleapis.com/token", token_request, cancelled, std::min<int64_t>(15, remaining));

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

        if(reply.result != CURLE_OK || reply.status == 429 || reply.status >= 500) {
            interval = std::min(std::max(interval, 5) * 2, 86400);
            continue;
        }

        return describe_error(reply);
    }

    return "Sign-in code expired. Select Connect to try again.";
}
} // namespace drive
