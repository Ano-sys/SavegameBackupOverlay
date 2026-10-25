#include "include/SaveGameListGui.hpp"

#include "include/BackupMenuGui.hpp"

SaveGameListGui::SaveGameListGui(AccountUid uid, u64 app_id, std::string user_name, std::string name) : BackupTaskGui(uid, app_id, std::move(user_name), name), user_uid(uid), app_id(app_id), name(std::move(name)) { task.list(); }

tsl::elm::Element* SaveGameListGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Google Drive Backups", name);
    list = new tsl::elm::List();

    auto* refresh = new tsl::elm::ListItem("Refresh");

    refresh->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(!task.view().busy) tsl::swapTo<SaveGameListGui>(user_uid, app_id, user_name, name);

        return true;
    });

    list->addItem(refresh);
    list->addItem(status_drawer(false), 100);
    frame->setContent(list);
    return frame;
}

void SaveGameListGui::update() {
    BackupTaskGui::update();

    if(!list || displayed || current_view.busy || !current_view.entries) return;

    displayed = true;

    for(const auto& entry : *current_view.entries) {
        std::string date = entry.created_time.substr(0, 19);

        if(date.size() > 10 && date[10] == 'T') date[10] = ' ';

        auto* item = new tsl::elm::ListItem(date + " UTC", entry.save_hash.substr(0, 8));

        item->setClickListener([uid = user_uid, application_id = app_id, user_name = user_name, name = name, entry](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            tsl::changeTo<BackupMenuGui>(uid, application_id, user_name, name, entry);
            return true;
        });

        list->addItem(item);
    }
}
