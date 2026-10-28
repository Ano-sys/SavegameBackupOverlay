#pragma once

#include "BackupTaskGui.hpp"

#include <optional>

class SaveGameListGui : public BackupTaskGui {
private:
    AccountUid user_uid;
    u64 app_id;
    std::string name;
    tsl::elm::List* list = nullptr;
    bool displayed = false;
    std::optional<backup::Identity> source;

public:
    SaveGameListGui(AccountUid uid, u64 app_id, std::string user_name, std::string name, std::optional<backup::Identity> source = std::nullopt, std::shared_ptr<const std::vector<backup::CloudBackup>> entries = {});
    tsl::elm::Element* createUI(void) override;
    void update() override;
};
