#include "include/BackupMenuGui.hpp"
#include "include/RestoreGui.hpp"
#include "include/DeleteBackupGui.hpp"

BackupMenuGui::BackupMenuGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry) : uid(uid), application_id(application_id), user_name(std::move(user_name)), name(std::move(name)), entry(std::move(entry)) {}

tsl::elm::Element* BackupMenuGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Cloud Backup", name);
    auto* list = new tsl::elm::List();
    std::string date = entry.created_time.substr(0, 19);

    if(date.size() > 10 && date[10] == 'T') date[10] = ' ';

    list->addItem(new tsl::elm::CategoryHeader(date + " UTC"));

    auto* restore = new tsl::elm::ListItem("Restore");

    restore->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        tsl::changeTo<RestoreGui>(uid, application_id, user_name, name, entry);
        return true;
    });

    list->addItem(restore);

    auto* remove = new tsl::elm::ListItem("Delete");

    remove->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        tsl::changeTo<DeleteBackupGui>(uid, application_id, user_name, name, entry);
        return true;
    });

    list->addItem(remove);
    frame->setContent(list);
    return frame;
}
