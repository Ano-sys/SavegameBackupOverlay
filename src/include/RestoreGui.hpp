#pragma once

#include "BackupTaskGui.hpp"

class RestoreGui : public BackupTaskGui {
public:
    RestoreGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry);
    tsl::elm::Element* createUI() override;
    void update() override;

private:
    std::string name;
    tsl::elm::List* list = nullptr;
    tsl::elm::ListItem* backup_item = nullptr;
    tsl::elm::ListItem* restore_item = nullptr;
    tsl::elm::ListItem* cancel_item = nullptr;
};
