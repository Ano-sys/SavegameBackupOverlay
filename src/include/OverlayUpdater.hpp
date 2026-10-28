#pragma once

#include "DriveStorage.hpp"

namespace updater {

inline constexpr const char* AssetName = "SavegameBackupOverlay.ovl";
inline constexpr uint64_t MaxOverlayBytes = 32 * 1024 * 1024;

struct Release {
    std::string tag;
    std::string overlay_url;
    std::string checksum_url;
    uint64_t size = 0;
};

std::string overlay_path(const std::string& argument);

class OverlayUpdater {
public:
    OverlayUpdater(drive::HttpTransport transport, backup::Control control = {});
    bool releases(unsigned page, std::vector<Release>& out, bool& has_more, std::string& error);
    // An empty tag selects GitHub's latest published full release.
    bool install(const std::string& tag, const std::string& target, std::string& installed_tag, std::string& error, const std::function<bool()>& begin_replace = {});

private:
    bool fetch(drive::HttpRequest request, drive::HttpReply& reply, std::string& error);
    drive::HttpTransport transport;
    backup::Control control;
};

} // namespace updater
