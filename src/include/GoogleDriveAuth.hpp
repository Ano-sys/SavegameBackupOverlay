#pragma once

#include "GoogleDriveConfig.hpp"

#include <switch.h>
#include <atomic>
#include <mutex>
#include <string>

namespace drive {

class GoogleDriveAuth {
public:
    struct View {
        bool busy = false;
        bool connected = false;

        bool uses_global = false;

        std::string message;
        std::string user_code;

        std::string verification_url;

        std::string qr_url;
        bool qr_includes_code = false;
    };

    explicit GoogleDriveAuth(std::string connection_key);
    ~GoogleDriveAuth();
    GoogleDriveAuth(const GoogleDriveAuth&) = delete;
    GoogleDriveAuth& operator=(const GoogleDriveAuth&) = delete;

    void connect();
    void disconnect();
    View view() const;

private:
    static void thread_entry(void* context);
    void start(bool connecting);
    void run();
    std::string authorize();
    void refresh_connection();

    std::string key;

    GoogleDriveConfig config;

    mutable std::mutex mutex;

    View current;

    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{true};

    Thread worker{};

    bool worker_created = false;
    bool connecting = false;
    bool disconnect_requested = false;
};
} // namespace drive
