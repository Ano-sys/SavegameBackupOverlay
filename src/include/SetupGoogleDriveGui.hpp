#pragma once

#include "GoogleDriveAuth.hpp"

#include <memory>
#include <optional>
#include <tesla.hpp>
#include <vector>

class SetupGoogleDriveGui : public tsl::Gui {
public:
    enum class DriveSetupState {
        Global,
        User,
    };

private:
    struct ConstructionKey {
    private:
        ConstructionKey() = default;
        friend class SetupGoogleDriveGui;
    };

    DriveSetupState state;

    std::optional<AccountUid> user_uid;
    std::unique_ptr<drive::GoogleDriveAuth> auth;

    drive::GoogleDriveAuth::View current_view;

    tsl::elm::ListItem* connect_item = nullptr;
    tsl::elm::ListItem* disconnect_item = nullptr;

    std::string qr_url;
    std::vector<uint8_t> qr;
    bool qr_valid = false;

    void connect(void);
    void disconnect(void);

public:
    explicit SetupGoogleDriveGui(ConstructionKey, DriveSetupState state, const AccountUid* uid = nullptr);
    [[nodiscard]] static std::unique_ptr<SetupGoogleDriveGui> create(DriveSetupState state, const AccountUid* uid = nullptr);
    static bool open(DriveSetupState state, const AccountUid* uid = nullptr);
    [[nodiscard]] tsl::elm::Element* createUI(void) override;
    void update() override;
};
