#pragma once

#include "BackupTaskGui.hpp"

class SaveGameListGui : public BackupTaskGui {
private:
    AccountUid user_uid;
    u64 app_id;
    std::string name;
    tsl::elm::List* list = nullptr;
    bool displayed = false;

public:
    SaveGameListGui(AccountUid uid, u64 app_id, std::string user_name, std::string name);
    tsl::elm::Element* createUI(void) override;
    void update() override;
};
