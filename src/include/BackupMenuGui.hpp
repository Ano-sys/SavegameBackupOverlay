#pragma once

#include "BackupTypes.hpp"

#include <tesla.hpp>

class BackupMenuGui : public tsl::Gui {
public:
    BackupMenuGui(AccountUid uid, u64 application_id, std::string user_name, std::string name, backup::CloudBackup entry);
    tsl::elm::Element* createUI() override;

private:
    AccountUid uid;
    u64 application_id;

    std::string user_name;
    std::string name;
    backup::CloudBackup entry;
};
