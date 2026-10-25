#include "include/GameListGui.hpp"
#include "include/GameMenuGui.hpp"

#include <cstdio>
#include <set>
#include <memory>
#include <string>
#include <cstring>

GameListGui::GameListGui(AccountUid uid, std::string user_name) : uid(uid), user_name(std::move(user_name)) {}

tsl::elm::Element* GameListGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Savegame Backup", "Select Game");

    auto* list = new tsl::elm::List();
    frame->setContent(list);

    FsSaveDataInfoReader reader{};
    Result rc = fsOpenSaveDataInfoReader(&reader, FsSaveDataSpaceId_User);

    if(R_FAILED(rc)) {
        list->addItem(new tsl::elm::ListItem("Saves could not be opened!"));
        return frame;
    }

    bool ns_ready = R_SUCCEEDED(nsInitialize());
    auto control = std::make_unique<NsApplicationControlData>();

    std::set<u64> seen;

    while(true) {
        FsSaveDataInfo info{};
        s64 count = 0;

        rc = fsSaveDataInfoReaderRead(&reader, &info, 1, &count);

        if(R_FAILED(rc) || count == 0) break;

        if(info.save_data_type != FsSaveDataType_Account) continue;
        if(info.uid.uid[0] != uid.uid[0] || info.uid.uid[1] != uid.uid[1]) continue;
        if(!seen.insert(info.application_id).second) continue;

        char titleId[17];
        std::snprintf(titleId, sizeof(titleId), "%016llX", static_cast<unsigned long long>(info.application_id));

        std::string name = titleId;

        if(ns_ready) {
            u64 actual_size = 0;
            NacpLanguageEntry* entry = nullptr;

            Result name_rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, info.application_id, control.get(), sizeof(NsApplicationControlData), &actual_size);

            if(R_SUCCEEDED(name_rc) && actual_size >= sizeof(NacpStruct) && R_SUCCEEDED(nsGetApplicationDesiredLanguage(&control->nacp, &entry)) && entry && entry->name[0]) {
                name.assign(entry->name, strnlen(entry->name, sizeof(entry->name)));
            }
        }

        auto* item = new tsl::elm::ListItem(name);

        item->setClickListener([user_uid = uid, application_id = info.application_id, user_name = user_name, name](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;

            tsl::changeTo<GameMenuGui>(user_uid, application_id, user_name, name);
            return true;
        });

        list->addItem(item);
    }

    if(ns_ready) nsExit();

    fsSaveDataInfoReaderClose(&reader);

    if(R_FAILED(rc)) {
        list->addItem(new tsl::elm::ListItem("Error!"));
    }
    else if(seen.empty()) {
        list->addItem(new tsl::elm::ListItem("No Saves Found!"));
    }

    return frame;
}
