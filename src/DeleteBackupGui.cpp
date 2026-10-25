#include "include/DeleteBackupGui.hpp"
#include "include/SaveGameListGui.hpp"

DeleteBackupGui::DeleteBackupGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry) : BackupTaskGui(uid, application_id, std::move(user_name), name), uid(uid), application_id(application_id), name(std::move(name)), entry(std::move(entry)) {}

tsl::elm::Element* DeleteBackupGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Delete Backup", name);
    auto* list = new tsl::elm::List();
    std::string date = entry.created_time.substr(0, 19);

    if(date.size() > 10 && date[10] == 'T') date[10] = ' ';

    auto* details = new tsl::elm::CustomDrawer([date, hash = entry.save_hash.substr(0, 8)](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32) {
        const auto color = renderer->a(tsl::defaultTextColor);
        renderer->drawString("Move this backup to Drive trash?", false, x, y + 24, 18, color, width);
        renderer->drawString(date + " UTC", false, x, y + 52, 18, color, width);
        renderer->drawString("Save hash: " + hash, false, x, y + 80, 16, color, width);
    });

    list->addItem(details, 100);

    auto* cancel = new tsl::elm::ListItem("Cancel");

    cancel->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(task.view().busy) task.cancel();
        else tsl::goBack();
        return true;
    });

    list->addItem(cancel);

    delete_item = new tsl::elm::ListItem("Delete", "Move to trash");

    delete_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        task.delete_backup(entry);
        return true;
    });

    list->addItem(delete_item);
    list->addItem(status_drawer(false), 100);
    frame->setContent(list);
    return frame;
}

void DeleteBackupGui::update() {
    BackupTaskGui::update();

    if(current_view.deleted && !current_view.busy) {
        tsl::swapTo<SaveGameListGui>(SwapDepth{3}, uid, application_id, user_name, name);
        return;
    }

    if(delete_item) delete_item->setValue(current_view.busy ? "Please wait" : "Move to trash");
}
