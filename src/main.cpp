#define TESLA_INIT_IMPL

#include <tesla.hpp>

#include "include/SettingGui.hpp"
#include "include/UserSelectGui.hpp"

static constexpr const char* APP_VERSION = "0.1.0";

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

            tsl::changeTo<SettingGui>();
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

int main(int argc, char** argv) { return tsl::loop<BackupOverlay>(argc, argv); }
