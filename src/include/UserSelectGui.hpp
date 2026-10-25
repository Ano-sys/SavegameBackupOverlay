#pragma once

#include <tesla.hpp>
#include <vector>
#include <string>

class UserSelectGui : public tsl::Gui {
public:
    struct User {
        std::string name;
        AccountUid uid;
    };

    static std::vector<User> get_users(void);

    tsl::elm::Element* createUI(void) override;
};
