#pragma once

#include "OverlayUpdater.hpp"
#include "BackgroundJob.hpp"

#include <tesla.hpp>
#include <mutex>

class UpdateGui : public tsl::Gui {
public:
    enum class Screen { Menu,
        Versions,
        Install };
    UpdateGui(std::string version, std::string path, Screen screen = Screen::Menu, std::string tag = {}, unsigned page = 1);
    ~UpdateGui();
    tsl::elm::Element* createUI() override;
    void update() override;
    bool handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) override;

private:
    struct State {
        bool busy = false;
        bool critical = false;
        bool installed = false;
        bool loaded = false;
        bool has_more = false;
        std::string message;
        uint64_t done = 0, total = 0;
        std::vector<updater::Release> releases;
    };

    void start();
    void run();
    void cancel();
    void reap();
    static void thread_entry(void* context);
    State view() const;

    std::string version, path, tag;
    Screen screen;
    unsigned page;
    mutable std::mutex mutex;
    State state, current;
    std::atomic<bool> cancelled{false};
    background::Job worker;
    bool displayed = false;
    tsl::elm::List* list = nullptr;
    tsl::elm::ListItem* back = nullptr;
};
