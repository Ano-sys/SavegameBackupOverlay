#include "include/SettingGui.hpp"
#include "include/SetupGoogleDriveGui.hpp"
#include "include/UserSelectGui.hpp"

namespace {
bool open_drive(SetupGoogleDriveGui::DriveSetupState state, const AccountUid* uid = nullptr) { return SetupGoogleDriveGui::open(state, uid); }
} // namespace

tsl::elm::Element* SettingGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Savegame Backup", "Settings");
    auto* list = new tsl::elm::List();

    auto* global = new tsl::elm::ListItem("Global Google Drive");

    global->setClickListener([](u64 keys) { return (keys & HidNpadButton_A) && open_drive(SetupGoogleDriveGui::DriveSetupState::Global); });

    list->addItem(global);
    list->addItem(new tsl::elm::CategoryHeader("Google Drive per user"));

    for(const auto& user : UserSelectGui::get_users()) {
        auto* item = new tsl::elm::ListItem(user.name);

        item->setClickListener([uid = user.uid](u64 keys) { return (keys & HidNpadButton_A) && open_drive(SetupGoogleDriveGui::DriveSetupState::User, &uid); });

        list->addItem(item);
    }

    frame->setContent(list);

    return frame;
}
