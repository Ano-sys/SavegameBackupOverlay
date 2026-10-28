#include "include/UpdateGui.hpp"
#include "include/Diagnostics.hpp"

#include <algorithm>

UpdateGui::UpdateGui(std::string version, std::string path, Screen screen, std::string tag, unsigned page) : version(std::move(version)), path(std::move(path)), tag(std::move(tag)), screen(screen), page(page) {
    state.message = "Choose Latest or Specific Version. Current version: v" + this->version + ".";
    if(screen != Screen::Menu) start();
}

UpdateGui::~UpdateGui() {
    cancel();
    worker.wait();
}

UpdateGui::State UpdateGui::view() const {
    std::lock_guard lock(mutex);
    return state;
}

void UpdateGui::cancel() {
    std::lock_guard lock(mutex);
    if(!state.critical) cancelled.store(true);
}

void UpdateGui::start() {
    if(view().busy) return;
    worker.wait();
    cancelled.store(false);
    {
        std::lock_guard lock(mutex);
        state = {};
        state.busy = true;
        state.message = "Connecting to GitHub ...";
    }
    std::string error;
    if(!worker.start(thread_entry, this, error)) {
        std::lock_guard lock(mutex);
        state.busy = false;
        state.message = error;
    }
}

void UpdateGui::reap() { worker.reap(); }

void UpdateGui::thread_entry(void* context) { static_cast<UpdateGui*>(context)->run(); }

void UpdateGui::run() {
    std::string error, installed_tag;
    std::vector<updater::Release> releases;
    bool has_more = false;
    bool ok = false;
    {
        drive::CurlSession network;
        const backup::Control control{&cancelled, [this](const std::string& message, uint64_t done, uint64_t total) {
                                          std::lock_guard lock(mutex);
                                          state.message = message;
                                          state.done = done;
                                          state.total = total;
                                      }};
        updater::OverlayUpdater updater([&](const drive::HttpRequest& request, const backup::Control& transfer) { return network.request(request, transfer); }, control);
        if(screen == Screen::Versions) ok = updater.releases(page, releases, has_more, error);
        else ok = updater.install(tag, path, installed_tag, error, [this] {
            std::lock_guard lock(mutex);
            if(cancelled.load()) return false;
            state.critical = true;
            return true;
        });
    }
    if(!ok) {
        if(error.empty()) error = "The update failed without diagnostic details.";
        error = backup::diagnostic_text(error);
        backup::log_error(error);
    }
    {
        std::lock_guard lock(mutex);
        state.busy = state.critical = false;
        state.done = state.total = 0;
        state.loaded = ok && screen == Screen::Versions;
        state.installed = ok && screen == Screen::Install;
        state.has_more = has_more;
        state.releases = std::move(releases);
        state.message = !ok ? error : state.installed      ? "Installed " + installed_tag + ". Close and reopen this overlay to use it."
                                  : state.releases.empty() ? "No installable release versions on this page."
                                                           : "Select a version to download and install.";
    }
}

tsl::elm::Element* UpdateGui::createUI() {
    auto* frame = new tsl::elm::OverlayFrame("Update Overlay", screen == Screen::Versions ? "Specific Version" : screen == Screen::Install ? (tag.empty() ? "Latest" : tag)
                                                                                                                                           : "Current: v" + version);
    list = new tsl::elm::List();
    if(screen == Screen::Menu) {
        auto* latest = new tsl::elm::ListItem("Latest");
        latest->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<UpdateGui>(version, path, Screen::Install);
            return true;
        });
        list->addItem(latest);
        auto* specific = new tsl::elm::ListItem("Specific Version");
        specific->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<UpdateGui>(version, path, Screen::Versions);
            return true;
        });
        list->addItem(specific);
    }
    list->addItem(new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width, s32) {
        const auto color = renderer->a(tsl::defaultTextColor);
        std::string line;
        unsigned lines = 0;
        for(size_t pos = 0; pos < current.message.size() && lines < 9;) {
            const unsigned char c = current.message[pos];
            const size_t length = c < 0x80 ? 1 : c < 0xE0 ? 2
                                             : c < 0xF0   ? 3
                                                          : 4;
            const auto next = current.message.substr(pos, length);
            if(!line.empty() && renderer->getTextDimensions(line + next, false, 18).first > width) {
                renderer->drawString(line, false, x, y + 24 + 22 * lines++, 18, color, width);
                line.clear();
            }
            line += next;
            pos += length;
        }
        if(!line.empty() && lines < 9) renderer->drawString(line, false, x, y + 24 + 22 * lines, 18, color, width);
        if(current.total) renderer->drawString(std::to_string(std::min<uint64_t>(100, current.done * 100 / current.total)) + "%", false, x, y + 244, 18, color, width);
    }),
        275);
    back = new tsl::elm::ListItem("Back");
    back->setClickListener([this](u64 keys) {
        if(!(keys & HidNpadButton_A)) return false;
        if(view().busy) cancel();
        else tsl::goBack();
        return true;
    });
    list->addItem(back);
    frame->setContent(list);
    update();
    return frame;
}

void UpdateGui::update() {
    reap();
    current = view();
    if(back) {
        back->setText(current.busy ? "Cancel" : "Back");
        back->setValue(current.critical ? "Please wait" : "");
    }
    if(!list || displayed || current.busy || !current.loaded) return;
    displayed = true;
    removeFocus();
    unsigned index = 0;
    if(page > 1) {
        auto* newer = new tsl::elm::ListItem("Newer releases");
        newer->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;
            tsl::swapTo<UpdateGui>(version, path, Screen::Versions, std::string(), page - 1);
            return true;
        });
        list->addItem(newer, 0, index++);
    }
    for(const auto& release : current.releases) {
        auto* item = new tsl::elm::ListItem(release.tag, release.tag == "v" + version ? "Current" : "Install");
        item->setClickListener([this, tag = release.tag](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;
            tsl::changeTo<UpdateGui>(version, path, Screen::Install, tag);
            return true;
        });
        list->addItem(item, 0, index++);
    }
    if(current.has_more) {
        auto* older = new tsl::elm::ListItem("Older releases");
        older->setClickListener([this](u64 keys) {
            if(!(keys & HidNpadButton_A)) return false;
            tsl::swapTo<UpdateGui>(version, path, Screen::Versions, std::string(), page + 1);
            return true;
        });
        list->addItem(older, 0, index);
    }
}

bool UpdateGui::handleInput(u64 keys_down, u64, const HidTouchState&, HidAnalogStickState, HidAnalogStickState) {
    if(!(keys_down & HidNpadButton_B) || !view().busy) return false;
    cancel();
    return true;
}
