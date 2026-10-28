#pragma once

#include <tesla.hpp>

class SettingGui : public tsl::Gui {
public:
    SettingGui(std::string version, std::string overlay_path) : version(std::move(version)), overlay_path(std::move(overlay_path)) {}
    tsl::elm::Element* createUI() override;

private:
    std::string version;
    std::string overlay_path;
};
