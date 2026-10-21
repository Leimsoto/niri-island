// niri-island-greeter: the login screen, as a greetd greeter.
//
//   niri-island-greeter            under greetd (talks to $GREETD_SOCK)
//   niri-island-greeter --demo     over the current desktop, to try it out
//   niri-island-greeter --wallpaper <file>

#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/util.hpp"
#include "core/wayland.hpp"
#include "greeter/greeter.hpp"
#include "ui/theme.hpp"

using namespace island;

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    signal(SIGPIPE, SIG_IGN);

    Greeter::Options opt;
    if (const char* s = getenv("GREETD_SOCK")) opt.socket = s;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--demo") opt.demo = true;
        else if (a == "--wallpaper" && i + 1 < argc) opt.wallpaper = argv[++i];
        else {
            std::cout << "uso: niri-island-greeter [--demo] [--wallpaper <archivo>]\n";
            return a == "-h" || a == "--help" ? 0 : 2;
        }
    }
    // The backdrop: given, the system's (set by the installer), or in the
    // demo the user's current one.
    if (opt.wallpaper.empty()) {
        for (std::string f : {std::string("/etc/niri-island/greeter-wallpaper.jpg"), std::string("/etc/niri-island/greeter-wallpaper.png")})
            if (file_exists(f)) opt.wallpaper = f;
        std::string st;
        if (opt.wallpaper.empty() && read_file(state_dir() + "/wallpaper.json", st)) {
            auto k = st.find("\"path\"");
            if (k != std::string::npos) {
                auto q1 = st.find('"', st.find(':', k) + 1), q2 = st.find('"', q1 + 1);
                if (q1 != std::string::npos && q2 != std::string::npos) opt.wallpaper = st.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    if (!opt.demo && opt.socket.empty()) {
        std::cerr << "niri-island-greeter: GREETD_SOCK no está definido (¿querías --demo?)\n";
        return 1;
    }

    Loop loop;  // the one loop (Loop::get() refers to it)
    Wayland wl;
    if (!wl.init()) {
        std::cerr << "niri-island-greeter: no hay compositor Wayland\n";
        return 1;
    }
    // The installer copies the user's palette next to the wallpaper, so the
    // login screen wears the same colours as the desktop.
    if (file_exists("/etc/niri-island/greeter-colors.json") && !opt.demo)
        config().palette_file = "/etc/niri-island/greeter-colors.json";
    Theme::get().load();
    Greeter greeter(wl, opt);
    greeter.on_done = [&loop] { loop.quit(); };
    loop.watch(wl.fd(), POLLIN, [&wl](short) { wl.read_events(); });
    loop.before_poll = [&] {
        wl.dispatch_pending();
        greeter.pump();
        greeter.pump_extras();
        wl.dispatch_pending();
        return greeter.wants_frame() || greeter.extras_want_frame() ? 0 : -1;
    };
    greeter.start();
    loop.run();
    return 0;
}
