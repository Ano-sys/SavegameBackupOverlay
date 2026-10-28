#include "include/SaveGameListGui.hpp"

#include "include/BackupMenuGui.hpp"

#include <algorithm>
#include <map>

SaveGameListGui::SaveGameListGui(AccountUid uid, u64 app_id, std::string user_name, std::string name, std::optional<backup::Identity> source, std::shared_ptr<const std::vector<backup::CloudBackup>> entries) : BackupTaskGui(uid, app_id, std::move(user_name), name), user_uid(uid), app_id(app_id), name(std::move(name)), source(std::move(source)) { task.list(std::move(entries)); }

tsl::elm::Element* SaveGameListGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame(source ? "Google Drive Backups" : "Backup Users", source ? backup::source_name(*source) + " / " + name : name);
    list = new tsl::elm::List();

    auto* refresh = new tsl::elm::ListItem("Refresh");

    refresh->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(!task.view().busy) tsl::swapTo<SaveGameListGui>(user_uid, app_id, user_name, name, source);

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

    if(!source) {
        std::map<std::pair<std::string, std::string>, backup::Identity> sources;
        for(const auto& entry : *current_view.entries) sources.emplace(std::make_pair(entry.source.user_id, entry.source.console_name), entry.source);
        std::vector<backup::Identity> sorted;
        for(const auto& [key, selected] : sources) sorted.push_back(selected);
        std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) { return backup::source_name(left) < backup::source_name(right); });
        for(const auto& selected : sorted) {
            const auto label = backup::source_name(selected);
            const bool duplicate = std::count_if(sorted.begin(), sorted.end(), [&](const auto& other) { return backup::source_name(other) == label; }) > 1;
            auto* item = new tsl::elm::ListItem(label, duplicate ? selected.user_id : "Saves");
            item->setClickListener([this, selected](u64 keys) {
                if(!(keys & HidNpadButton_A)) return false;
                tsl::changeTo<SaveGameListGui>(user_uid, app_id, user_name, name, selected, current_view.entries);
                return true;
            });
            list->addItem(item);
        }
        return;
    }

    const auto snapshot = current_view.entries;
    std::vector<size_t> indices;
    for(size_t index = 0; index < snapshot->size(); ++index) {
        const auto& entry = (*snapshot)[index];
        if(entry.source.user_id == source->user_id && entry.source.console_name == source->console_name) indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end(), [&](size_t left, size_t right) { return (*snapshot)[left].created_time > (*snapshot)[right].created_time; });
    for(const auto index : indices) {
        const auto& entry = (*snapshot)[index];
        std::string date = entry.created_time.substr(0, 19);

        if(date.size() > 10 && date[10] == 'T') date[10] = ' ';

        auto* item = new tsl::elm::ListItem(date + " UTC", entry.save_hash.substr(0, 8));

        item->setClickListener([this, snapshot, index](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            tsl::changeTo<BackupMenuGui>(user_uid, app_id, user_name, name, (*snapshot)[index]);
            return true;
        });

        list->addItem(item);
    }
}
