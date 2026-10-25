#include "include/UserSelectGui.hpp"
#include "include/GameListGui.hpp"

#include <cstring>

std::vector<UserSelectGui::User> UserSelectGui::get_users(void) {
    std::vector<User> users{};

    if(R_FAILED(accountInitialize(AccountServiceType_System))) return users;

    AccountUid uids[ACC_USER_LIST_SIZE]{};
    s32 count = 0;

    if(R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &count))) {
        for(s32 i = 0; i < count; ++i) {
            AccountProfile profile{};

            if(R_FAILED(accountGetProfile(&profile, uids[i]))) continue;

            AccountProfileBase base{};
            Result rc = accountProfileGet(&profile, nullptr, &base);
            accountProfileClose(&profile);

            if(R_FAILED(rc)) continue;

            users.push_back({std::string(base.nickname, strnlen(base.nickname, sizeof(base.nickname))), uids[i]});
        }
    }

    accountExit();
    return users;
}

tsl::elm::Element* UserSelectGui::createUI() {
    auto frame = new tsl::elm::OverlayFrame("Savegame Backup", "Select User");

    auto list = new tsl::elm::List();
    auto users = get_users();

    for(const auto& user : users) {
        auto item = new tsl::elm::ListItem(user.name);

        AccountUid uid = user.uid;

        item->setClickListener([uid, name = user.name](u64 keys) {
            if(keys & HidNpadButton_A) {
                tsl::changeTo<GameListGui>(uid, name);
                return true;
            }

            return false;
        });

        list->addItem(item);
    }

    frame->setContent(list);

    return frame;
}
