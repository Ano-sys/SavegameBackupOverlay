#define TESLA_INIT_IMPL

#include <tesla.hpp>

#include "include/SettingGui.hpp"
#include "include/UserSelectGui.hpp"
#include "include/OverlayUpdater.hpp"
#include "include/BackgroundJob.hpp"
#include "include/Diagnostics.hpp"

extern "C" {
// The default 32 entries reserve about 25 KiB per open directory. Four entries
// keep directory scans small enough for the overlay's limited heap.
u32 __nx_fsdev_direntry_cache_size = 4;
}

static constexpr const char* APP_VERSION = "0.2.0";
static std::string overlay_path;

class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto* frame = new tsl::elm::OverlayFrame("Savegame Cloud Backup", APP_VERSION);
        auto* list = new tsl::elm::List();

        auto* users = new tsl::elm::ListItem("User Selection");

        users->setClickListener([](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            tsl::changeTo<UserSelectGui>();
            return true;
        });

        list->addItem(users);

        auto* settings = new tsl::elm::ListItem("Settings");

        settings->setClickListener([](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            tsl::changeTo<SettingGui>(APP_VERSION, overlay_path);
            return true;
        });

        list->addItem(settings);

        frame->setContent(list);

        return frame;
    }
};

class BackupOverlay : public tsl::Overlay {
public:
    std::unique_ptr<tsl::Gui> loadInitialGui() override { return std::make_unique<MainGui>(); }
};

int main(int argc, char** argv) {
    if(argc > 0 && argv && argv[0]) overlay_path = updater::overlay_path(argv[0]);
    const int result = tsl::loop<BackupOverlay>(argc, argv);
    std::string worker_error;
    if(!background::shutdown(worker_error)) backup::log_error(worker_error);
    drive::CurlSession::shutdown();
    return result;
}
