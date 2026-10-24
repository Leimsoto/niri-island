// Commands from the socket (`niri-island <command>`), for niri key binds.

#include <sstream>

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

static std::vector<std::string> args_of(const std::string& cmd) {
    // Splits on spaces, honouring "double quotes".
    std::vector<std::string> out;
    std::string cur;
    bool q = false, any = false;
    for (char c : cmd) {
        if (c == '"') {
            q = !q;
            any = true;
        } else if (c == ' ' && !q) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

std::string App::command(const std::string& line) {
    auto a = args_of(line);
    if (a.empty()) return "error: empty";
    const std::string& c = a[0];
    std::string rest;
    for (size_t i = 1; i < a.size(); ++i) rest += (i > 1 ? " " : "") + a[i];
    request();

    auto toggle_panel = [&](View v) {
        if (!nav_.empty() && nav_.back() == v) {
            close_all();
            return std::string("closed");
        }
        open(v, true);
        return std::string("opened");
    };

    // panels
    if (c == "toggle-control-center" || c == "cc" || c == "control") return toggle_panel(View::Control);
    if (c == "launcher" || c == "toggle-launcher" || c == "run") {
        if (rest.empty()) return toggle_panel(View::Launcher);
        // With a query it always opens (or re-fills) instead of toggling.
        open(View::Launcher, true);
        query_.set(rest);
        sel_ = 0;
        refresh_launcher();
        return "opened";
    }
    if (c == "wifi" || c == "open-wifi") return toggle_panel(View::Wifi);
    if (c == "bt" || c == "bluetooth" || c == "open-bt") return toggle_panel(View::Bluetooth);
    if (c == "sound" || c == "audio") return toggle_panel(View::Sound);
    if (c == "weather") return toggle_panel(View::Weather);
    if (c == "calendar" || c == "cal") return toggle_panel(View::Calendar);
    if (c == "tasks" || c == "board") return toggle_panel(View::Board);
    if (c == "notes") return toggle_panel(View::Notes);
    if (c == "stats" || c == "system") return toggle_panel(View::Stats);
    if (c == "session" || c == "power-menu") return toggle_panel(View::Session);
    if (c == "task-add" || c == "add-task") {
        // task-add [--date YYYY-MM-DD] title…
        std::string date = Store::today(), title = rest;
        if (a.size() > 2 && a[1] == "--date") {
            date = a[2];
            title.clear();
            for (size_t i = 3; i < a.size(); ++i) title += (i > 3 ? " " : "") + a[i];
        }
        int id = store_.add_task(title, date);
        return id >= 0 ? "task " + std::to_string(id) : "error: empty title";
    }
    if (c == "note-new" || c == "quick-note") {
        int id = store_.add_note("", rest, static_cast<int>(store_.notes().size() % 5));
        if (id < 0) return "error: could not create the note";
        open(View::Notes, true);  // Escape returns to the deck
        open_note(id);
        return "note " + std::to_string(id);
    }
    if (c == "capture") {
        // the bubble at the bottom; "capture record" opens it on recording
        int mode = a.size() > 1 && (a[1] == "record" || a[1] == "grabar") ? 1 : 0;
        if (capbar_.is_open() && !capbar_leaving_ && (a.size() < 2 || mode == capbar_mode_)) {
            close_capbar();
            return "closed";
        }
        start_capture(mode);
        return "opened";
    }
    if (c == "screenshot" || c == "shot") {
        // screenshot [region|screen|window] [delay] [--no-edit]
        std::string mode = a.size() > 1 ? a[1] : "region";
        int delay = 0;
        bool edit = true;
        for (size_t i = 2; i < a.size(); ++i) {
            if (a[i] == "--no-edit") edit = false;
            else delay = std::clamp(std::atoi(a[i].c_str()), 0, 30);
        }
        if (mode == "window") {
            const Window* fw = niri_.focused_window();
            if (!fw) return "error: no focused window";
            take_screenshot(2, delay, fw->id);
        } else {
            take_screenshot(mode == "screen" ? 1 : 0, delay, 0, edit);
        }
        return "ok";
    }
    if (c == "record") {
        std::string sub = a.size() > 1 ? a[1] : "toggle";
        if (sub == "stop") recorder_.stop();
        else if (sub == "cancel") recorder_.cancel();
        else if (sub == "pause") recorder_.pause();
        else if (sub == "resume") recorder_.resume();
        else if (sub == "region" || sub == "screen") {
            recorder_.options().target = sub == "region" ? 1 : 0;
            start_recording();
        } else if (sub == "start") start_recording();
        else if (recorder_.active()) recorder_.stop();
        else start_recording();
        return recorder_.active() ? "recording" : "idle";
    }
    if (c == "wallpaper") {
        std::string op = a.size() > 1 ? a[1] : "current";
        if (op == "next") wallpaper_next(1);
        else if (op == "prev") wallpaper_next(-1);
        else if (op == "set" && a.size() > 2) {
            std::string path = expand_home(rest.substr(rest.find("set") + 4));
            if (!file_exists(path)) return "error: no such file";
            backdrop_.set(path);
        } else if (op != "current") return "usage: wallpaper [current|next|prev|set <path>]";
        return backdrop_.current();
    }
    if ((c == "wallpapers" || c == "walls" || c == "wallpaper-picker") && wallpicker_.is_open()) {
        wallpicker_.dismiss();  // the same key closes it
        return "closed";
    }
    if (c == "wallpapers" || c == "walls" || c == "wallpaper-picker") {
        open_wallpaper_picker();
        if (a.size() > 1 && a[1] == "wallhaven") wallpicker_.show_wallhaven();  // straight to the downloads
        else if (a.size() > 1 && a[1] == "engine") wallpicker_.show_engine();
        return "opened";
    }
    if (c == "clipboard") return toggle_panel(View::Clipboard);
    if (c == "notifications" || c == "inbox") return toggle_panel(View::Inbox);
    if (c == "tray") return toggle_panel(View::Tray);
    if (c == "tray-list") {
        std::string out;
        for (auto& it : tray_.items()) out += it.key + "  " + it.name + "  [" + it.icon_name + "]\n";
        return out.empty() ? "(vacía)" : out;
    }
    if (c == "screensaver") {
        std::string op = a.size() > 1 ? a[1] : "start";
        if (op == "stop") stop_screensaver();
        else start_screensaver();
        return "ok";
    }
    if (c == "lock") {
        double preview = 0;
        if (a.size() > 2 && a[1] == "--preview") preview = std::max(1.0, std::atof(a[2].c_str()));
        lock_screen(preview);
        return "locking";
    }
    if (c == "lock-state") return lock_.locked() ? "locked" : lock_.active() ? "locking" : "unlocked";
    if (c == "night-light") {
        proc::spawn_shell(config().cmd_night_light);
        return "ok";
    }
    if (c == "debug-edge-click" && a.size() >= 4) {
        (a[1] == "left" ? bar_left_ : bar_right_)
            .inject_click(std::stof(a[2]), std::stof(a[3]), a.size() > 4 && a[4] == "right" ? Button::Right : Button::Left);
        return "ok";
    }
    if (c == "debug-picker-click" && a.size() >= 3) {
        wallpicker_.inject_click(std::stof(a[1]), std::stof(a[2]));
        return "ok";
    }
    if (c == "debug-demo") {
        demo_ = a.size() < 2 || a[1] != "off";
        lock_.name_override = demo_ ? "Usuario" : "";
        request();
        return demo_ ? "demo on" : "demo off";
    }
    if (c == "debug-idle" && a.size() >= 2) {
        // the screensaver's idle watch, in seconds, to test it (setup_idle()
        // puts the configured one back on the next settings change)
        idle_key_.clear();
        idle_saver_ = wl_.watch_idle(static_cast<uint32_t>(std::atoi(a[1].c_str())) * 1000u,
                                     [this] { start_screensaver(); }, [this] { stop_screensaver(); });
        return idle_saver_ ? "ok" : "no idle notifier";
    }
    if (c == "debug-settings-search") {
        std::string q;
        for (size_t i = 1; i < a.size(); ++i) q += (i > 1 ? " " : "") + a[i];
        set_search_.text = q;
        set_search_.caret = q.size();
        settings_.request();
        return "ok";
    }
    if (c == "debug-settings-scroll" && a.size() >= 4) {
        settings_.inject_scroll(std::stof(a[1]), std::stof(a[2]), std::stof(a[3]));
        return "ok";
    }
    if (c == "debug-capbar-click" && a.size() >= 3) {
        capbar_.inject_click(std::stof(a[1]), std::stof(a[2]));
        return capbar_.is_open() ? "open" : "closed";
    }
    if (c == "debug-settings-click" && a.size() >= 3) {
        settings_.inject_click(std::stof(a[1]), std::stof(a[2]));
        return "ok";
    }
    if (c == "settings") {
        // Optional section: appearance bar glass notifications capture wallpapers system about
        static const char* names[] = {"appearance", "bar", "glass", "notifications", "capture", "wallpapers", "system", "about"};
        if (settings_.is_open() && a.size() == 1) {
            settings_.close_now();  // the same key closes it
            return "closed";
        }
        open_settings();
        if (a.size() > 1)
            for (int i = 0; i < 8; ++i)
                if (a[1] == names[i]) set_section_ = i;
        settings_.request();
        return "opened";
    }
    // Compatibility with the previous command names.
    if (c == "toggle-media" || c == "expand") return command("glance");
    if (c == "mock-notify") return command("notify " + rest);
    if (c == "wifi-status") return command("wifi-list");
    if (c == "rescan-wifi") return command("wifi-scan");
    if (c == "cal-next" || c == "cal-prev" || c == "cal-today") {
        if (c == "cal-today") cc_month_offset_ = cal_offset_ = 0;
        else {
            int d = c == "cal-next" ? 1 : -1;
            cc_month_offset_ += d;
            cal_offset_ += d;
        }
        return "ok";
    }
    if ((c == "note-open" || c == "open-note") && a.size() > 1) {
        int id = std::atoi(a[1].c_str());
        if (!store_.note(id)) return "error: no note " + a[1];
        open(View::Notes, true);
        open_note(id);
        return "ok";
    }
    if (c == "glance" || c == "peek") {
        if (panel_open()) close_all();
        summary_ = !summary_;
        if (summary_) {
            // Stays until the pointer leaves the island or 6 s pass.
            loop_.after(6.0, [this] {
                if (!pointer_over_island_) {
                    summary_ = false;
                    request();
                }
            });
        }
        return summary_ ? "glance on" : "glance off";
    }
    if (c == "close") {
        close_all();
        notifs_.dismiss_current();
        osd_.until = 0;
        return "closed";
    }
    if (c == "back") {
        back();
        return "ok";
    }

    // media
    if (c == "play-pause") return mpris_.play_pause(), "ok";
    if (c == "next") return mpris_.next(), "ok";
    if (c == "prev" || c == "previous") return mpris_.previous(), "ok";

    // levels
    auto step_of = [&](float def) { return a.size() > 1 ? std::atof(a[1].c_str()) / 100.0f : def; };
    if (c == "volume-up") return audio_.change_volume(step_of(0.05f)), "ok";
    if (c == "volume-down") return audio_.change_volume(-step_of(0.05f)), "ok";
    if (c == "volume" && a.size() > 1) return audio_.set_volume(std::atof(a[1].c_str()) / 100.0f), "ok";
    if (c == "toggle-mute" || c == "mute") return audio_.toggle_mute(), "ok";
    if (c == "toggle-mic" || c == "mic-mute") return audio_.toggle_mic_mute(), "ok";
    if (c == "brightness-up" || c == "brightness-down") {
        // From the keys: show the bubble (the setter mutes the uevent it causes,
        // which is right for the slider but not for the keys).
        power_.change_brightness(c == "brightness-up" ? step_of(0.05f) : -step_of(0.05f));
        if (power_.on_brightness) power_.on_brightness(true);
        return "ok";
    }

    // toggles
    if (c == "toggle-wifi") return net_.set_wifi(!net_.state().wifi_enabled), "ok";
    if (c == "toggle-bluetooth" || c == "toggle-bt") return bt_.set_powered(!bt_.state().powered), "ok";
    if (c == "toggle-dnd" || c == "dnd") {
        std::string m = a.size() > 1 ? a[1] : "";
        if (m == "off") notifs_.set_dnd_mode(Notifications::DndOff);
        else if (m == "discreet" || m == "discreto") notifs_.set_dnd_mode(Notifications::DndDiscreet);
        else if (m == "total" || m == "on") notifs_.set_dnd_mode(Notifications::DndTotal);
        else if (m == "cycle") notifs_.cycle_dnd();
        else notifs_.set_dnd(!notifs_.dnd());
        return std::string("dnd ") + Notifications::dnd_label(notifs_.dnd_mode());
    }
    if (c == "toggle-airplane" || c == "airplane") return set_airplane(!airplane()), "ok";
    if (c == "power-profile") {
        if (a.size() > 1) power_.set_profile(a[1]);
        else power_.cycle_profile();
        return power_.profile();
    }

    // timer
    if (c == "timer") {
        if (a.size() == 1) return toggle_panel(View::Timer);
        if (a[1] == "pause" || a[1] == "toggle") return timer_.toggle(), "ok";
        if (a[1] == "cancel" || a[1] == "stop") return timer_.cancel(), "ok";
        std::string label;
        double secs = Timer::parse(rest, &label);
        if (secs <= 0) return "error: duration like 25m, 1h30, 90s, 5:00";
        timer_.start(secs, label);
        return "timer " + fmt_duration(static_cast<int64_t>(secs));
    }

    if (c == "stopwatch") {
        std::string op = a.size() > 1 ? a[1] : "toggle";
        if (op == "toggle") stopwatch_.toggle();
        else if (op == "lap") stopwatch_.lap();
        else if (op == "reset") stopwatch_.reset();
        else if (op == "open") {
            open(View::Timer);
            timer_mode(2, {});
        } else return "usage: stopwatch [toggle|lap|reset|open]";
        return "ok";
    }

    // notifications
    if (c == "notify" || c == "test-notif") {
        // notify "App" "Summary" "Body"
        std::string app = a.size() > 1 ? a[1] : "niri-island";
        std::string sum = a.size() > 2 ? a[2] : "Hola";
        std::string body = a.size() > 3 ? a[3] : "Esto es una notificación de prueba.";
        notifs_.post_local(app, sum, body, "dialog-information");
        return "ok";
    }
    if (c == "notifications-clear") return notifs_.clear_history(), "ok";
    if (c == "osd-test") {
        OsdInfo o;
        o.kind = OsdInfo::Volume;
        o.value = audio_.state().sink.volume;
        o.glyph = "󰕾";
        o.label = "Prueba";
        o.detail = audio_.state().sink.description;
        show_osd(o);
        return "ok";
    }
    if (c == "battery" || c == "battery-osd") {
        OsdInfo o;
        o.kind = OsdInfo::Battery;
        o.label = power_.battery().charging ? "Cargando" : "Batería";
        show_osd(o, 3.0);
        return "ok";
    }

    // Pointer injection for testing, in island-relative coordinates.
    if ((c == "debug-move" || c == "debug-click") && a.size() >= 3) {
        Rect ir = island_rect();
        float x = ir.x + static_cast<float>(std::atof(a[1].c_str()));
        float y = ir.y + static_cast<float>(std::atof(a[2].c_str()));
        pointer_in_ = true;
        on_motion(x, y);
        if (c == "debug-click") {
            Button b = a.size() > 3 && a[3] == "right" ? Button::Right : a.size() > 3 && a[3] == "middle" ? Button::Middle
                                                                                                      : Button::Left;
            on_button(b, true);
            on_button(b, false);
        }
        return "at " + std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(y));
    }
    if (c == "debug-drag" && a.size() >= 5) {
        Rect ir = island_rect();
        float x1 = ir.x + std::atof(a[1].c_str()), y1 = ir.y + std::atof(a[2].c_str());
        float x2 = ir.x + std::atof(a[3].c_str()), y2 = ir.y + std::atof(a[4].c_str());
        pointer_in_ = true;
        on_motion(x1, y1);
        on_button(Button::Left, true);
        for (int i = 1; i <= 10; ++i) on_motion(x1 + (x2 - x1) * i / 10.0f, y1 + (y2 - y1) * i / 10.0f);
        on_button(Button::Left, false);
        return "dragged";
    }
    if (c == "reload") {
        config().load();
        Spring::speed = config().motion;
        paint::reset_fonts();
        Theme::get().load();
        ImageCache::get().clear();
        weather_.reload();
        if (island_) island_->set_margin(config().margin_top);
        return "reloaded";
    }

    if (c == "status") {
        std::ostringstream ss;
        const auto& m = mpris_.info();
        const auto& s = audio_.state();
        ss << "view: " << static_cast<int>(shown_) << (panel_open() ? " (panel)" : "") << "\n";
        ss << "media: " << (m.active ? m.artist + " — " + m.title + (m.playing ? " (playing)" : " (paused)") : "none") << "\n";
        ss << "volume: " << static_cast<int>(std::lround(s.sink.volume * 100)) << "%" << (s.sink.muted ? " muted" : "")
           << " on " << s.sink.description << "\n";
        ss << "mic: " << (s.source.muted ? "muted" : "live");
        if (!s.mic_apps.empty()) ss << " (in use by " << s.mic_apps.front() << ")";
        ss << "\n";
        ss << "wifi: " << (net_.state().wifi_enabled ? (net_.state().ssid.empty() ? "on, disconnected" : net_.state().ssid) : "off")
           << "\n";
        ss << "bluetooth: " << (bt_.state().powered ? "on" : "off") << ", " << bt_.state().devices.size() << " devices\n";
        ss << "battery: " << power_.battery().percent << "% " << power_.battery().status << "\n";
        ss << "notifications: " << notifs_.mode_label() << ", " << notifs_.history().size() << " in history"
           << (notifs_.dnd() ? ", dnd" : "") << "\n";
        ss << "workspaces: " << niri_.output_workspaces().size() << (niri_.connected() ? "" : " (niri not connected)") << "\n";
        ss << "timer: " << (timer_.running() ? fmt_duration((int64_t)timer_.remaining()) : "off") << "\n";
        ss << "weather: " << (weather_.data().valid ? std::to_string(weather_.data().temp) + "° " + weather_.data().condition +
                                                          " (" + weather_.source() + ")"
                                                    : "n/a")
           << "\n";
        return ss.str();
    }
    if (c == "wifi-list") {
        std::ostringstream ss;
        for (auto& n : net_.state().networks)
            ss << (n.in_use ? "* " : "  ") << n.ssid << "  " << n.signal << "%  " << (n.security.empty() ? "open" : n.security)
               << (n.saved ? "  saved" : "") << "\n";
        return ss.str().empty() ? "no networks (open the panel or run wifi-scan)" : ss.str();
    }
    if (c == "wifi-scan") return net_.scan(true), "scanning";
    if (c == "wifi-connect" && a.size() > 1) {
        net_.connect(a[1], a.size() > 2 ? a[2] : "");
        return "connecting to " + a[1];
    }
    if (c == "wifi-disconnect") return net_.disconnect(), "ok";
    if (c == "wifi-password" && a.size() > 1) {
        if (nav_.empty()) open(View::Wifi, true);
        open_password(a[1]);
        return "password prompt for " + a[1];
    }
    if (c == "wifi-forget" && a.size() > 1) return net_.forget(a[1]), "ok";

    if (c == "help" || c == "--help") {
        return "panels: cc launcher [query] wifi bt sound weather calendar tasks notes note-new [text] stats session clipboard notifications tray timer stopwatch settings [section] glance close back\n"
               "suite: wallpapers wallpaper [current|next|prev|set <path>] screenshot record screensaver [stop] lock [--preview <s>] lock-state night-light\n"
               "media: play-pause next prev\n"
               "levels: volume-up [n] volume-down [n] volume <n> mute mic-mute brightness-up [n] brightness-down [n]\n"
               "toggles: toggle-wifi toggle-bluetooth toggle-dnd [off|discreet|total|cycle] toggle-airplane power-profile [name]\n"
               "timer: timer <25m|1h30|5:00> [label] | timer pause | timer cancel\n"
               "wifi: wifi-list wifi-scan wifi-connect <ssid> [pass] wifi-password <ssid> wifi-disconnect wifi-forget <ssid>\n"
               "tasks: task-add [--date YYYY-MM-DD] <title>\n"
               "capture: capture screenshot [region|screen|window] [delay] [--no-edit] record [toggle|start|region|screen|pause|resume|stop|cancel] wallpapers\n"
               "other: notify <app> <summary> <body> notifications-clear osd-test battery reload status";
    }
    return "unknown command: " + c + " (try: niri-island help)";
}

}  // namespace island
