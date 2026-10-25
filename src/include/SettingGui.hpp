#pragma once

#include <tesla.hpp>

class SettingGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override;
};
