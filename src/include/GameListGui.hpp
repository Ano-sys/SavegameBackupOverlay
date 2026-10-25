#pragma once

#include <tesla.hpp>

class GameListGui : public tsl::Gui {
private:
    AccountUid uid;
    std::string user_name;

public:
    GameListGui(AccountUid uid, std::string user_name);

    tsl::elm::Element* createUI() override;
};
