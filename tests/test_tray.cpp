#include "check.hpp"
#include "services/tray.hpp"

using namespace island;

static std::string name(std::string id, std::string title = "", std::string proc = "") {
    TrayItem it;
    it.id = std::move(id);
    it.title = std::move(title);
    it.proc = std::move(proc);
    return tray_display_name(it);
}

TEST(tray_names) {
    CHECK_EQ(name("discord_status_icon_1"), std::string("Discord"));
    CHECK_EQ(name("nm-applet"), std::string("Nm"));                     // "-applet" goes
    CHECK_EQ(name("chrome_status_icon_12"), std::string("Chrome"));
    CHECK_EQ(name("xpad", "Xpad"), std::string("Xpad"));                // a title wins
    CHECK_EQ(name("StatusNotifierItem-1", "", "telegram"), std::string("Telegram"));  // nothing left: the process
    CHECK_EQ(name("steam"), std::string("Steam"));
}
