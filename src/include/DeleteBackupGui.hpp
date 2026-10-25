#pragma once

#include "BackupTaskGui.hpp"

class DeleteBackupGui : public BackupTaskGui {
public:
    DeleteBackupGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry);
    tsl::elm::Element* createUI() override;
    void update() override;

private:
    AccountUid uid;
    u64 application_id;

    std::string name;
    backup::CloudBackup entry;

    tsl::elm::ListItem* delete_item = nullptr;
};
