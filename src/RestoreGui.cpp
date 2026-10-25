#include "include/RestoreGui.hpp"

RestoreGui::RestoreGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry) : BackupTaskGui(uid, application_id, std::move(user_name), name), name(std::move(name)) { task.prepare_restore(std::move(entry)); }

tsl::elm::Element* RestoreGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Restore Backup", name);
    list = new tsl::elm::List();
    list->addItem(status_drawer(), 280);

    cancel_item = new tsl::elm::ListItem("Cancel");

    cancel_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(task.view().busy) task.cancel();
        else tsl::goBack();
        return true;
    });

    list->addItem(cancel_item);
    frame->setContent(list);
    update();
    return frame;
}

void RestoreGui::update() {
    BackupTaskGui::update();

    if(list && current_view.question && !current_view.busy && !backup_item) {
        removeFocus();
        backup_item = new tsl::elm::ListItem("Back up current save and restore");

        backup_item->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            task.continue_restore(backup::RestoreChoice::BackupFirst);
            return true;
        });

        list->addItem(backup_item, 0, 1);

        restore_item = new tsl::elm::ListItem("Restore without cloud backup");

        restore_item->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            task.continue_restore(backup::RestoreChoice::WithoutBackup);
            return true;
        });

        list->addItem(restore_item, 0, 2);
    }

    if(list && !current_view.busy && !current_view.question && backup_item) {
        removeFocus();
        list->removeItem(backup_item);
        list->removeItem(restore_item);
        backup_item = restore_item = nullptr;
    }

    const std::string value = current_view.busy ? "Please wait" : "Select";

    if(backup_item) backup_item->setValue(value);

    if(restore_item) restore_item->setValue(value);

    if(cancel_item) {
        cancel_item->setText(current_view.busy || current_view.question ? "Cancel" : "Back");
        cancel_item->setValue(current_view.critical ? "Please wait" : "");
    }
}
