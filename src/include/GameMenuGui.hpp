#pragma once

#include "BackupTaskGui.hpp"

class GameMenuGui : public BackupTaskGui {
public:
    GameMenuGui(AccountUid uid, u64 application_id, std::string user_name, std::string name);
    tsl::elm::Element* createUI() override;
    void update() override;

private:
    AccountUid uid;
    u64 application_id;
    std::string name;
    tsl::elm::ListItem* backup_item = nullptr;
    tsl::elm::ListItem* restore_item = nullptr;
};
