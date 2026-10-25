#include "include/GameMenuGui.hpp"
#include "include/SaveGameListGui.hpp"

GameMenuGui::GameMenuGui(AccountUid uid, u64 application_id, std::string user_name, std::string name) : BackupTaskGui(uid, application_id, std::move(user_name), name), uid(uid), application_id(application_id), name(std::move(name)) {}

tsl::elm::Element* GameMenuGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Savegame Backup", name);
    auto* list = new tsl::elm::List();

    restore_item = new tsl::elm::ListItem("Restore");

    restore_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(!task.view().busy) tsl::changeTo<SaveGameListGui>(uid, application_id, user_name, name);

        return true;
    });

    list->addItem(restore_item);

    backup_item = new tsl::elm::ListItem("Backup");

    backup_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        task.backup();
        return true;
    });

    list->addItem(backup_item);
    list->addItem(status_drawer(), 280);
    frame->setContent(list);
    return frame;
}

void GameMenuGui::update() {
    BackupTaskGui::update();

    if(backup_item) backup_item->setValue(current_view.busy ? "Please wait" : "Upload new");

    if(restore_item) restore_item->setValue(current_view.busy ? "Please wait" : "Google Drive");
}
