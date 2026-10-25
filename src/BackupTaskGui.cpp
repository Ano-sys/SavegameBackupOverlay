#include "include/BackupTaskGui.hpp"

#include <algorithm>
#include <vector>

namespace {
std::vector<std::string> wrap_text(tsl::gfx::Renderer* renderer, const std::string& text, s32 width, u32 size, size_t limit) {
    std::vector<std::string> lines;
    std::string line;

    for(size_t pos = 0; pos < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2
                                         : c < 0xF0   ? 3
                                                      : 4;
        const auto next = text.substr(pos, length);

        if(!line.empty() && renderer->getTextDimensions(line + next, false, size).first > width) {
            lines.push_back(line);
            line.clear();

            if(lines.size() == limit) return lines;
        }

        if(!line.empty() || next != " ") line += next;

        pos += length;
    }

    if(!line.empty()) lines.push_back(line);

    return lines;
}
} // namespace

BackupTaskGui::BackupTaskGui(AccountUid uid, u64 application_id, std::string user_name, std::string application_name) : user_name(std::move(user_name)), task(uid, application_id, this->user_name, std::move(application_name)) {}

void BackupTaskGui::update() {
    current_view = task.view();
    task.reap_finished();
}

bool BackupTaskGui::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) {
    if(!(keys_down & HidNpadButton_B)) return false;

    const auto state = task.view();

    if(!state.busy) return false;
    if(!state.critical) task.cancel();

    return true;
}

tsl::elm::CustomDrawer* BackupTaskGui::status_drawer(bool show_recovery) {
    return new tsl::elm::CustomDrawer([this, show_recovery, previous = std::string(), previous_path = std::string(), previous_width = s32{-1}, previous_budget = s32{-1}, lines = std::vector<std::string>(), path_lines = std::vector<std::string>()](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32 height) mutable {
        const auto color = renderer->a(tsl::defaultTextColor);
        const s32 reserved = 24 + (show_recovery && !current_view.recovery_path.empty() ? 116 : 0) + (current_view.total ? 24 : 0) + (current_view.critical ? 24 : 0);
        const s32 line_budget = std::clamp<s32>((height - reserved) / 22, 1, 12);

        if(previous != current_view.message || previous_path != current_view.recovery_path || previous_width != width || previous_budget != line_budget) {
            previous = current_view.message;
            previous_path = current_view.recovery_path;
            previous_width = width;
            previous_budget = line_budget;
            lines = wrap_text(renderer, previous, width, 18, line_budget);
            path_lines = show_recovery ? wrap_text(renderer, previous_path, width, 14, 5) : std::vector<std::string>{};
        }

        s32 baseline = y + 24;

        for(const auto& line : lines) {
            renderer->drawString(line, false, x, baseline, 18, color, width);
            baseline += 22;
        }

        if(current_view.total) {
            const auto percent = std::min<uint64_t>(100, current_view.done * 100 / current_view.total);
            renderer->drawString(std::to_string(percent) + "%", false, x, baseline, 18, color, width);
            baseline += 24;
        }

        if(current_view.critical) {
            renderer->drawString("Writing save. Please wait.", false, x, baseline, 16, color, width);
            baseline += 24;
        }

        if(show_recovery && !path_lines.empty()) {
            renderer->drawString("Local recovery ZIP:", false, x, baseline + 6, 16, color, width);
            baseline += 26;

            for(const auto& line : path_lines) {
                renderer->drawString(line, false, x, baseline, 14, color, width);
                baseline += 18;
            }
        }
    });
}
