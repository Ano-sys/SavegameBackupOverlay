#include "include/SetupGoogleDriveGui.hpp"
#include "third_party/qrcodegen/qrcodegen.h"

#include <algorithm>
#include <array>
#include <cstdio>

namespace {
std::vector<std::string> wrap_text(tsl::gfx::Renderer* renderer, const std::string& text, s32 width, u32 font_size, size_t max_lines) {
    std::vector<std::string> lines;
    std::string line;

    for(size_t pos = 0; pos < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2
                                         : c < 0xF0   ? 3
                                                      : 4;

        const std::string next = text.substr(pos, length);

        if(!line.empty() && renderer->getTextDimensions(line + next, false, font_size).first > width) {
            lines.push_back(line);
            line.clear();

            if(lines.size() == max_lines) return lines;
        }

        if(!line.empty() || next != " ") line += next;

        pos += length;
    }

    if(!line.empty()) lines.push_back(line);

    return lines;
}
} // namespace

std::unique_ptr<SetupGoogleDriveGui> SetupGoogleDriveGui::create(DriveSetupState state, const AccountUid* uid) {
    if(state == DriveSetupState::User && (!uid || !accountUidIsValid(uid))) return nullptr;

    return std::unique_ptr<SetupGoogleDriveGui>(new SetupGoogleDriveGui(ConstructionKey{}, state, uid));
}

bool SetupGoogleDriveGui::open(DriveSetupState state, const AccountUid* uid) {
    if(state == DriveSetupState::User && (!uid || !accountUidIsValid(uid))) return false;

    tsl::changeTo<SetupGoogleDriveGui>(ConstructionKey{}, state, uid);
    return true;
}

SetupGoogleDriveGui::SetupGoogleDriveGui(ConstructionKey, DriveSetupState state, const AccountUid* uid) : state(state) {
    std::string key = "global";

    if(state == DriveSetupState::User) {
        user_uid = *uid;
        char id[33];
        std::snprintf(id, sizeof(id), "%016llx%016llx", static_cast<unsigned long long>(uid->uid[0]), static_cast<unsigned long long>(uid->uid[1]));
        key = std::string("user:") + id;
    }

    auth = std::make_unique<drive::GoogleDriveAuth>(std::move(key));
}

void SetupGoogleDriveGui::connect(void) {
    auth->connect();
    update();
}

void SetupGoogleDriveGui::disconnect(void) {
    auth->disconnect();
    update();
}

tsl::elm::Element* SetupGoogleDriveGui::createUI(void) {
    auto* frame = new tsl::elm::OverlayFrame("Google Drive", state == DriveSetupState::Global ? "Global connection" : "User connection");
    auto* list = new tsl::elm::List();

    connect_item = new tsl::elm::ListItem("Connect");

    connect_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        connect();
        return true;
    });

    list->addItem(connect_item);

    disconnect_item = new tsl::elm::ListItem("Disconnect");

    disconnect_item->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;

        disconnect();
        return true;
    });

    list->addItem(disconnect_item);

    auto* instructions = new tsl::elm::CustomDrawer([this, previous_message = std::string(), previous_url = std::string(), previous_width = s32{-1}, message_lines = std::vector<std::string>(), url_lines = std::vector<std::string>()](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32) mutable {
        const auto color = renderer->a(tsl::defaultTextColor);

        if(previous_message != current_view.message || previous_url != current_view.verification_url || previous_width != width) {
            previous_message = current_view.message;
            previous_url = current_view.verification_url;
            previous_width = width;
            message_lines = wrap_text(renderer, previous_message, width, 18, 3);
            url_lines = wrap_text(renderer, previous_url, width, 16, 2);
        }

        s32 baseline = y + 24;

        for(const auto& line : message_lines) {
            renderer->drawString(line, false, x, baseline, 18, color, width);
            baseline += 22;
        }

        if(current_view.user_code.empty()) return;

        u32 code_size = 28;

        while(code_size > 12 && renderer->getTextDimensions(current_view.user_code, true, code_size).first > width) --code_size;

        const s32 code_width = renderer->getTextDimensions(current_view.user_code, true, code_size).first;

        renderer->drawString(current_view.user_code, true, x + std::max(0, (width - code_width) / 2), y + 100, code_size, color, width);

        baseline = y + 124;

        for(const auto& line : url_lines) {
            renderer->drawString(line, false, x, baseline, 16, color, width);
            baseline += 18;
        }

        if(qr_valid) {
            const int modules = qrcodegen_getSize(qr.data());
            const int scale = std::max(1, std::min(width, 180) / (modules + 8));
            const int side = (modules + 8) * scale;
            const s32 left = x + (width - side) / 2;
            const s32 top = y + 155;

            renderer->drawRect(left, top, side, side, renderer->a(tsl::Color(15, 15, 15, 15)));

            for(int row = 0; row < modules; ++row)
                for(int col = 0; col < modules; ++col)
                    if(qrcodegen_getModule(qr.data(), col, row)) renderer->drawRect(left + (col + 4) * scale, top + (row + 4) * scale, scale, scale, renderer->a(tsl::Color(0, 0, 0, 15)));
        }
        else {
            renderer->drawString("QR code unavailable. Use the URL.", false, x, y + 190, 16, color, width);
        }

        renderer->drawString(current_view.qr_includes_code ? "The QR link includes the code." : "Enter the code on the Google page.", false, x, y + 354, 16, color, width);
    });

    list->addItem(instructions, 365);
    frame->setContent(list);
    update();

    return frame;
}

void SetupGoogleDriveGui::update() {
    const auto next = auth->view();

    if(connect_item && (next.busy != current_view.busy || next.connected != current_view.connected || next.uses_global != current_view.uses_global || current_view.message.empty())) {
        connect_item->setValue(next.busy ? "Please wait" : next.connected ? "Connected"
                                                                          : "Sign in");
        disconnect_item->setValue(next.busy ? "Cancel" : next.uses_global ? "Global remains"
                                                                          : "");
    }

    current_view = next;

    if(qr_url != current_view.qr_url) {
        qr_url = current_view.qr_url;
        qr_valid = false;

        if(!qr_url.empty()) {
            constexpr int maximum_version = 10;
            std::array<uint8_t, qrcodegen_BUFFER_LEN_FOR_VERSION(maximum_version)> temporary{};

            qr.resize(temporary.size());
            qr_valid = qrcodegen_encodeText(qr_url.c_str(), temporary.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, maximum_version, qrcodegen_Mask_AUTO, true);
        }
    }
}
