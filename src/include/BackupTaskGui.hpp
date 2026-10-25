#pragma once

#include "BackupTask.hpp"

#include <tesla.hpp>

class BackupTaskGui : public tsl::Gui {
public:
    BackupTaskGui(AccountUid uid, u64 application_id, std::string user_name, std::string application_name);
    void update() override;
    bool handleInput(u64 keys_down, u64 keys_held, const HidTouchState& touch, HidAnalogStickState left, HidAnalogStickState right) override;

protected:
    tsl::elm::CustomDrawer* status_drawer(bool show_recovery = true);
    std::string user_name;
    backup::BackupTask task;
    backup::BackupTask::View current_view;
};
