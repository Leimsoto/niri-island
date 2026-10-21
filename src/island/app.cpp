#include "island/app.hpp"

#include <iostream>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pango/pangocairo.h>

#include "core/config.hpp"
#include "core/proc.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

App::App() = default;

App::~App() {
    for (auto& [k, v] : shadow_cache_) cairo_surface_destroy(v);
    if (snap_prev_) cairo_surface_destroy(snap_prev_);
    if (snap_next_) cairo_surface_destroy(snap_next_);
    proc::g_shutdown = true;
    flush_note();
    cava_.set_running(false);
}

bool App::init() {
    config().load();
    Spring::speed = config().motion;
    Theme::get().load();

    if (!wl_.init()) return false;
    wire_wayland();
    create_surface();
    backdrop_.init();
    // The picker's list, thumbnails and slices, ready before it is first opened.
    loop_.after(4.0, [this] { wallpicker_.prewarm(); });

    user_bus_.open_user();
    sys_bus_.open_system();

    ImageCache::get().on_ready = [this] {
        request();
        wallpicker_.request();
    };
    wallpicker_.on_toast = [this](const std::string& m) { show_toast(m); };
    wallpicker_.on_apply = [this](const std::string& path) { backdrop_.set(path); };
    backdrop_.on_palette = [this] {
        Theme::get().load();
        request();
    };
    backdrop_.on_error = [this](const std::string& m) { show_toast(m); };
    lock_.on_failed = [] { proc::spawn_shell("swaylock -f -c 141218"); };
    backdrop_.on_shown = [this](const std::string& path) { wallpicker_.backdrop_shown(path); };
    capture_.on_toast = [this](const std::string& m) { show_toast(m); };
    capture_.on_saved = [this](const std::string& path, cairo_surface_t* thumb, bool copied) {
        if (path.empty()) {
            if (thumb) cairo_surface_destroy(thumb);
            show_toast(copied ? "Captura copiada" : "No se pudo guardar la captura");
            return;
        }
        notify_file("Captura guardada", path, thumb, copied ? "Copiada al portapapeles" : "");
    };
    recorder_.init();
    recorder_.on_change = [this] { request(); };
    recorder_.on_countdown = [this](int left) {
        OsdInfo o;
        o.kind = OsdInfo::Message;
        o.value = -1;
        o.glyph = "󰑋";
        o.label = "Grabando en " + std::to_string(left) + "…";
        o.detail = "niri-island record cancel para cancelar";
        show_osd(o, 1.1);
    };
    recorder_.on_done = [this](const std::string& path, bool ok, const std::string& err) {
        if (!ok) {
            notifs_.post_local("Grabador", "La grabación falló", err.empty() ? "Revisa wf-recorder" : err, "media-record", 2);
            return;
        }
        auto thumb = std::make_shared<cairo_surface_t*>(nullptr);
        std::string p = path;
        loop_.async(
            [p, thumb] {
                std::string jpg = cache_dir() + "/rec-thumb.jpg";
                proc::run({"ffmpeg", "-y", "-v", "error", "-ss", "0.5", "-i", p, "-frames:v", "1", "-vf", "scale=320:-2", jpg},
                          15000);
                *thumb = load_image_blocking(jpg, 160);
                unlink(jpg.c_str());
            },
            [this, p, thumb] {
                struct stat st {};
                std::string size;
                if (stat(p.c_str(), &st) == 0) {
                    char b[32];
                    snprintf(b, sizeof(b), "%.1f MB", st.st_size / 1048576.0);
                    size = b;
                }
                notify_file("Grabación guardada", p, *thumb, size);
            });
    };

    if (user_bus_) {
        mpris_.init(user_bus_.get());
        notifs_.init(user_bus_.get());
        tray_.init(user_bus_.get());
    }
    audio_.init();
    power_.init(sys_bus_.get());
    net_.init(sys_bus_.get());
    bt_.init(sys_bus_.get());
    niri_.init();
    weather_.init();
    store_.open();
    store_.cleanup_done(86400);
    apps_.init();
    wire_services();

    if (!ipc_.start([this](const std::string& c) { return command(c); })) {
        std::cerr << "niri-island is already running\n";
        return false;
    }

    files_.watch(Theme::get().palette_path(), [this] {
        Theme::get().load();
        request();
    });
    files_.watch(expand_home(config().weather_cache), [this] { weather_.reload(); });
    files_.watch(Config::path(), [this] {
        config().load();
        Spring::speed = config().motion;
        paint::reset_fonts();
        Theme::get().load();
        ++config_gen_;
        apply_layout();
        request();
    });

    loop_.before_poll = [this] { return before_poll(); };
    // Warm the font and layout caches off the critical path, so the first
    // control centre or launcher opens as smoothly as every later one.
    loop_.after(1.5, [this] {
        for (View v : {View::Control, View::Launcher, View::Glance, View::Notes, View::Calendar, View::Sound}) {
            float w = 0, h = 0;
            sizes_for(v, w, h);
            if (cairo_surface_t* s = snapshot(v, w, h)) cairo_surface_destroy(s);
        }
    });
    // A one-second heartbeat: the clock, timers, polling while visible.
    loop_.after(1.0 - std::fmod(now_mono(), 1.0), [this] {
        tick_second();
        loop_.after(1.0, [this] { tick_second(); }, 1.0);
    });
    return true;
}

void App::run() { loop_.run(); }

void App::create_surface() {
    LayerSpec spec;
    spec.width = 0;  // the whole width: panels can open under a corner button
    spec.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
    spec.height = kSurfaceH;
    spec.margin_top = attached() ? 0 : config().margin_top;
    // Windows keep clear of the resting island (the zone counts after the margin).
    spec.exclusive_zone = 0;  // set by apply_layout() right after
    spec.name = "niri-island";
    island_ = wl_.create_layer(spec);
    LayerSurface* s = island_.get();
    s->on_configure = [this] { request(); };
    s->on_closed = [this] {
        std::cerr << "[island] surface closed (output change), recreating\n";
        wl_.set_shield(false);
        loop_.post([this] {
            island_.reset();
            loop_.after(0.5, [this] { create_surface(); });
        });
    };
    s->on_pointer_enter = [this](double x, double y) {
        pointer_in_ = true;
        on_motion(x, y);
    };
    s->on_pointer_leave = [this] {
        pointer_in_ = false;
        pointer_over_island_ = false;
        ui_.leave();
        loop_.cancel(hover_timer_);
        hover_timer_ = 0;
        summary_ = false;
        request();
    };
    s->on_pointer_motion = [this](double x, double y) { on_motion(x, y); };
    s->on_button = [this](Button b, bool pressed) { on_button(b, pressed); };
    s->on_scroll = [this](double dx, double dy, bool discrete) { on_scroll(dx, dy, discrete); };
    s->on_key = [this](xkb_keysym_t sym, const std::string& utf8, uint32_t mods) { on_key(sym, utf8, mods); };
    last_input_.clear();
    apply_layout();
    request();
}

void App::wire_wayland() {
    settings_.painter = [this](cairo_t* cr, Ui& u, float w, float h, float dt) { draw_settings(cr, u, w, h, dt); };
    settings_.on_key = [this](xkb_keysym_t s, const std::string& t, uint32_t m) { settings_key(s, t, m); };
    settings_.on_backdrop_click = [this](float, float) { settings_.close_now(); };
    settings_.moving = [this] { return !set_scroll_.settled() || !set_search_.text.empty() || !set_edit_key_.empty(); };
    for (EdgeBar* b : {&bar_left_, &bar_right_}) {
        b->painter = [this](cairo_t* cr, Ui& u, float w, float h, bool left, std::vector<Rect>& input) {
            paint_edge(cr, u, w, h, left, input);
        };
        b->moving = [this] { return !ws_pill_.settled(); };
    }
    wl_.on_outside_click = [this] {
        if (osd_pinned_) {
            // The battery card (opened by a click) goes as promptly as a panel.
            osd_pinned_ = false;
            osd_.until = 0;
            wl_.set_shield(false);
        }
        close_all();
        request();
    };
    loop_.watch(wl_.fd(), POLLIN, [this](short) { wl_.read_events(); });
}

void App::wire_services() {
    mpris_.on_change = [this] { request(); };
    notifs_.on_arrive = [this](const NotificationPtr&) {
        notif_expanded_ = false;
        request();
    };
    notifs_.on_change = [this] { request(); };
    tray_.on_change = [this] { request(); };
    notifs_.on_discreet = [this](const NotificationPtr& n) {
        // A soft sound and the sender's icon, shaking for a moment beside the island.
        ping_icon_ = !n->app_icon.empty() ? "icon:" + n->app_icon
                   : !n->desktop_entry.empty() ? "icon:" + n->desktop_entry
                   : starts_with(n->image, "icon:") ? n->image : "";
        ping_pixels_ = ping_icon_.empty() ? n->pixels : nullptr;
        ping_app_ = n->app;
        ping_at_ = now_mono();
        ping_alpha_.set(1);
        loop_.cancel(ping_timer_);
        ping_timer_ = loop_.after(1.8, [this] {
            ping_timer_ = 0;
            ping_alpha_.set(0);
            request();
        });
        const char* snd = "/usr/share/sounds/freedesktop/stereo/message.oga";
        if (proc::has("paplay") && file_exists(snd)) proc::spawn({"paplay", snd});
        request();
    };

    audio_.on_sink_change = [this](bool user) {
        if (user) {
            const auto& s = audio_.state().sink;
            OsdInfo o;
            o.kind = OsdInfo::Volume;
            o.value = std::min(1.0f, s.volume);
            o.muted = s.muted;
            o.glyph = s.muted || s.volume < 0.005f ? "󰝟" : s.volume < 0.34f ? "󰕿" : s.volume < 0.67f ? "󰖀" : "󰕾";
            o.label = s.muted ? "Silenciado" : std::to_string(static_cast<int>(std::lround(s.volume * 100))) + "%";
            o.detail = s.description;
            show_osd(o);
        }
        request();
    };
    audio_.on_mic_mute = [this](bool muted) {
        OsdInfo o;
        o.kind = OsdInfo::Mic;
        o.value = -1;
        o.glyph = muted ? "󰍭" : "󰍬";
        o.label = muted ? "Micrófono silenciado" : "Micrófono activo";
        o.muted = muted;
        show_osd(o, 1.4);
    };
    audio_.on_change = [this] { request(); };

    power_.on_brightness = [this](bool user) {
        if (user) {
            OsdInfo o;
            o.kind = OsdInfo::Brightness;
            o.value = power_.brightness();
            o.glyph = o.value < 0.34f ? "󰃞" : o.value < 0.67f ? "󰃟" : "󰃠";
            o.label = std::to_string(static_cast<int>(std::lround(o.value * 100))) + "%";
            o.detail = "Brillo";
            show_osd(o);
        }
        request();
    };
    power_.on_plug = [this](bool plugged) {
        OsdInfo o;
        o.kind = OsdInfo::Battery;
        o.value = power_.battery().percent / 100.0f;
        o.glyph = plugged ? "󰂄" : "󰁹";
        o.label = plugged ? "Cargando" : "Con batería";
        show_osd(o, 3.2);
    };
    power_.on_low = [this](int pct) {
        notifs_.post_local("Batería", "Batería baja: " + std::to_string(pct) + "%",
                           pct <= 5 ? "Conecta el cargador ahora." : "Conecta el cargador pronto.", "battery-caution",
                           pct <= 10 ? 2 : 1);
    };
    power_.on_change = [this] { request(); };

    net_.on_change = [this] { request(); };
    net_.on_password_needed = [this](const std::string& ssid) { open_password(ssid); };
    net_.on_connected = [this](const std::string& ssid) {
        if (!nav_.empty() && (nav_.back() == View::WifiPassword)) back();
        show_toast("Conectado a " + ssid);
    };
    bt_.on_change = [this] { request(); };
    bt_.on_device_connection = [this](const BtDevice& d, bool connected) {
        if (panel_open()) return;
        OsdInfo o;
        o.kind = OsdInfo::Message;
        o.value = d.battery >= 0 ? d.battery / 100.0f : -1;
        o.glyph = connected ? "󰂱" : "󰂲";
        o.label = d.name;
        o.detail = connected ? (d.battery >= 0 ? "Conectado · " + std::to_string(d.battery) + "%" : "Conectado")
                             : "Desconectado";
        show_osd(o, 2.4);
    };
    niri_.on_change = [this] { request(); };
    weather_.on_change = [this] { request(); };
    apps_.on_change = [this] {
        if (shown_ == View::Launcher) refresh_launcher();
        request();
    };
    timer_.on_change = [this] { request(); };
    stopwatch_.on_change = [this] { request(); };
    timer_.on_finish = [this](const std::string& label) {
        notifs_.post_local("Temporizador", label.empty() ? "Tiempo cumplido" : label, "La cuenta atrás ha terminado.",
                           "alarm-clock", 2);
        if (proc::has("paplay") && file_exists("/usr/share/sounds/freedesktop/stereo/complete.oga"))
            proc::spawn({"paplay", "/usr/share/sounds/freedesktop/stereo/complete.oga"});
    };
}

// ── loop ───────────────────────────────────────────────────────────────────

int App::before_poll() {
    int timeout = -1;
    auto merge = [&](int t) {
        if (t >= 0) timeout = timeout < 0 ? t : std::min(timeout, t);
    };
    merge(user_bus_.pump());
    merge(sys_bus_.pump());
    wl_.dispatch_pending();
    backdrop_.pump();
    wallpicker_.pump();
    capture_.pump();
    if (dirty_) {
        // The corner strips redraw only when what they show changed (they
        // animate their own hovers); not every time the island does.
        std::string sig = bar_signature();
        if (sig != bar_sig_ || !ws_pill_.settled()) {
            bar_sig_ = sig;
            bar_left_.request();
            bar_right_.request();
        }
    }
    bar_left_.pump();
    bar_right_.pump();
    settings_.pump();
    screensaver_.pump();
    lock_.pump();
    if (dirty_ && island_ && island_->configured() && !island_->frame_pending()) {
        render();
        wl_.dispatch_pending();
        // That dispatch may have released an overlay's frame: draw it now,
        // instead of sleeping until the next unrelated wake-up.
        wallpicker_.pump();
        capture_.pump();
        bar_left_.pump();
        bar_right_.pump();
        settings_.pump();
        screensaver_.pump();
        lock_.pump();
    }
    for (Overlay* o : std::initializer_list<Overlay*>{&wallpicker_, &capture_, &bar_left_, &bar_right_, &settings_, &screensaver_})
        if (o->wants_frame()) merge(0);
    if (lock_.wants_frame()) merge(0);
    // Overlays commit after the dispatch above flushed: send their frames now,
    // or they sit in the socket buffer until the island next draws.
    wl_.dispatch_pending();
    return timeout;
}

void App::tick_second() {
    // Repaint when something time-based is visible.
    std::string key = fmt_time(config().clock_24h ? "%H:%M" : "%I:%M");
    bool minute = key != clock_key_;
    clock_key_ = key;
    bool seconds_visible = shown_ == View::Glance || shown_ == View::Music || shown_ == View::Timer ||
                           shown_ == View::Control || shown_ == View::Stats || clock_active();
    if (minute || seconds_visible || mpris_.info().playing) request();

    // Sample the system while someone is looking at it.
    if (shown_ == View::Control || shown_ == View::Stats || shown_ == View::Glance) sysmon_.sample();
    static int n = 0;
    ++n;
    if (n % 30 == 0) power_.poll();
    if (n % 10 == 0) notifs_.health_check();
    if (n % 600 == 0) store_.cleanup_done(86400);
    if ((shown_ == View::Control || shown_ == View::Wifi) && n % 5 == 0) net_.refresh();
}

// ── arbitration ────────────────────────────────────────────────────────────

View App::decide() const {
    if (!nav_.empty()) return nav_.back();
    if (notifs_.current()) return View::Notification;
    if (osd_.until > now_mono()) return View::Osd;
    if (summary_) return mpris_.info().active ? View::Music : View::Glance;
    return View::Rest;
}

void App::open(View v, bool replace) {
    if (replace) nav_.clear();
    if (!nav_.empty() && nav_.back() == v) return;
    // A fresh panel is centred unless a corner button just asked for it;
    // panels opened from inside an anchored one stay where they are.
    if (nav_.empty() && !anchor_pending_) anchored_ = false;
    anchor_pending_ = false;
    // A panel takes over: whatever was flashing is noise now.
    osd_.until = 0;
    summary_ = false;
    if (notifs_.current()) notifs_.dismiss_current();
    nav_.push_back(v);
    request();
}

void App::back() {
    if (nav_.empty()) return;
    nav_.pop_back();
    request();
}

void App::close_all() {
    nav_.clear();
    summary_ = false;
    request();
}

void App::on_view_enter(View v) {
    switch (v) {
        case View::Timer:
            // Straight into whatever is running; otherwise the choice.
            tm_mode_ = timer_.running() ? 1 : stopwatch_.running() ? 2 : 0;
            tm_anim_.reset(1);
            break;
        case View::Control:
            sysmon_.sample();
            net_.refresh();
            cc_notif_scroll_.reset(0);
            break;
        case View::Wifi:
            wifi_scroll_.reset(0);
            if (net_.state().wifi_enabled) net_.scan(false);
            break;
        case View::Bluetooth:
            bt_scroll_.reset(0);
            break;
        case View::Launcher:
            // The query is cleared on leave, so `niri-island launcher <text>` can pre-fill it.
            sel_ = 0;
            clip_items_.clear();
            launcher_scroll_.reset(0);
            refresh_launcher();
            break;
        case View::Stats: sysmon_.sample(); break;
        case View::Calendar:
            task_input_ = false;
            task_scroll_.reset(0);
            break;
        case View::Board:
            board_input_ = false;
            board_notes_focus_ = false;
            drag_task_ = -1;
            break;
        case View::Notes: notes_scroll_.reset(0); break;
        case View::Inbox: inbox_scroll_.reset(0); break;
        case View::Tray:
            if (!tray_menu_pending_) tray_menu_key_.clear();
            tray_menu_pending_ = false;
            tray_menu_path_.clear();
            break;
        case View::Clipboard:
            clip_query_.clear();
            clip_scroll_.reset(0);
            clip_list_scroll_.reset(0);
            clip_search_focus_ = true;
            clip_sel_id_.clear();
            // Remember who had focus: closing lets focus-follows-mouse move it.
            clip_target_win_ = 0;
            clip_target_app_.clear();
            if (const auto* win = niri_.focused_window()) {
                clip_target_win_ = win->id;
                clip_target_app_ = win->app_id;
            }
            clip_wipe_armed_ = 0;
            load_clipboard();
            break;
        case View::Session:
            confirm_.clear();
            session_sel_ = 0;
            break;
        case View::Capture: cap_windows_ = false; break;
        default: break;
    }
}

void App::on_view_leave(View v) {
    switch (v) {
        case View::NoteEditor: flush_note(); break;
        case View::WifiPassword: pass_field_.clear(); break;
        case View::Launcher: query_.clear(); break;
        case View::Board:
            if (board_notes_focus_ && board_detail_ >= 0) store_.set_task_notes(board_detail_, board_notes_.text);
            board_notes_focus_ = false;
            break;
        default: break;
    }
}

bool App::text_input_active() const {
    switch (shown_) {
        case View::Launcher:
        case View::WifiPassword:
        case View::NoteEditor:
        case View::Session: return true;  // arrows move between the actions
        case View::Clipboard: return true;  // arrows and type-to-search; the paste refocuses the window
        case View::Calendar: return task_input_;
        case View::Board: return board_input_ || board_notes_focus_;
        default: return false;
    }
}

void App::update_keyboard() {
    if (island_) island_->set_keyboard(text_input_active() ? KeyboardMode::Exclusive : KeyboardMode::None);
}

void App::sizes_for(View v, float& w, float& h) {
    const auto& m = mpris_.info();
    switch (v) {
        case View::Rest: {
            w = 128;
            h = bar_h();
            bool media = m.active;
            bool tm = clock_active();
            bool mic = audio_.mic_in_use();
            if (media || tm || mic) w = 204;
            if (tm && (media || mic)) w = 226;
            if (recorder_.active()) w = 226;
            w += 2 * one_island_side();
            break;
        }
        case View::Glance: w = 372; h = clock_active() ? 152 : 92; break;
        case View::Music: w = 392; h = clock_active() ? 220 : 164; break;
        case View::Osd:
            w = osd_.kind == OsdInfo::Battery || osd_.kind == OsdInfo::Volume || osd_.kind == OsdInfo::Brightness ? 340 : 320;
            h = osd_.kind == OsdInfo::Battery ? 138 : osd_.kind == OsdInfo::Volume ? 100 : osd_.kind == OsdInfo::Brightness ? 72 : 60;
            break;
        case View::Notification: {
            auto n = notifs_.current();
            w = 420;
            h = 78;
            if (n) {
                bool inline_actions = n->actions.size() <= 2;
                size_t letters = 0;
                for (auto& a : n->actions) letters += a.second.size();
                if (letters > 18) inline_actions = false;
                if (!n->actions.empty() && !inline_actions) h += 46;
                if (notif_expanded_ && !n->body.empty()) {
                    Text t{ui(12.5f), {}, Align::Left, w - 90, 0, true, true};
                    Size s = measure(n->body, t);
                    h += std::clamp(s.h - 34.0f, 0.0f, 220.0f);
                }
                if (n->value >= 0) h += 14;
            }
            break;
        }
        case View::Control: w = 840; h = 548; break;
        case View::Wifi: {
            w = 410;
            size_t n = net_.state().wifi_enabled ? net_.state().networks.size() : 0;
            h = std::clamp(62.0f + std::max<size_t>(n, 3) * 62.0f + 14.0f, 280.0f, 520.0f);
            break;
        }
        case View::WifiPassword: w = 410; h = 214; break;
        case View::WifiInfo: w = 450; h = wifi_qr_ ? 600 : 420; break;
        case View::Bluetooth: {
            w = 410;
            size_t n = bt_.state().powered ? bt_.state().devices.size() : 0;
            h = std::clamp(62.0f + std::max<size_t>(n, 3) * 62.0f + 14.0f, 280.0f, 520.0f);
            break;
        }
        case View::Sound: {
            w = 480;
            const auto& st = audio_.state();
            float content = 0;
            if (snd_tab_ == 0) content = std::max<size_t>(1, st.sinks.size()) * 92.0f;
            else if (snd_tab_ == 1) content = std::max<size_t>(1, st.sources.size()) * 92.0f;
            else content = std::max<size_t>(1, st.streams.size()) * (st.sinks.size() > 1 ? 122.0f : 92.0f);
            h = 108 + std::min(content, 4 * 92.0f) + 8;
            break;
        }
        case View::Weather: w = 500; h = 350; break;
        case View::Clipboard: w = 780; h = 520; break;
        case View::Inbox: {
            w = 420;
            float content = 0;
            for (auto& n : notifs_.history()) content += 58 + (n->body.empty() ? 0 : 34);
            h = std::clamp(102 + content + 16, 220.0f, 560.0f);
            break;
        }
        case View::Tray: {
            w = 340;
            if (!tray_menu_key_.empty()) {
                const std::vector<TrayMenuItem>* lv = &tray_menu_;
                for (int idx : tray_menu_path_)
                    if (idx >= 0 && idx < static_cast<int>(lv->size())) lv = &(*lv)[idx].children;
                float c = 0;
                for (auto& m : *lv) c += !m.visible ? 0 : m.separator ? 11 : 38;
                h = std::clamp(60 + c + 12, 120.0f, 560.0f);
            } else {
                size_t n = tray_.items().size();
                h = n ? std::min(560.0f, 62 + n * 54.0f + 8) : 160;
            }
            break;
        }
        case View::Calendar: w = 780; h = 460; break;
        case View::DatePicker: w = 360; h = 430; break;
        case View::Board: w = 880; h = 540; break;
        case View::Notes: {
            w = 580;
            size_t rows = (store_.notes().size() + 1 + 2) / 3;
            h = std::min(480.0f, 64.0f + rows * 172.0f + 8.0f);
            // While a note flies back the island keeps the paper's size.
            if (note_collapse_.value > 0.02f) h = std::max(h, lerp(h, 480.0f, note_collapse_.value));
            break;
        }
        case View::NoteEditor: w = 580; h = 480; break;
        case View::Launcher: {
            w = 620;
            size_t rows = std::min<size_t>(results_.size(), 8);
            h = 70 + (rows ? rows * 50.0f + 12 : 44);
            break;
        }
        case View::Session: w = 560; h = 164; break;
        case View::Stats: w = 560; h = 360; break;
        case View::Timer:
            w = 400;
            if (tm_mode_ == 0) h = 196;
            else if (tm_mode_ == 1) h = timer_.running() ? 214 : 322;
            else h = stopwatch_.laps().empty() ? 214 : std::min(214.0f + 14 + stopwatch_.laps().size() * 30.0f, 360.0f);
            break;
        case View::Capture: w = 600; h = cap_windows_ ? 440 : 486; break;
        case View::Record: w = 440; h = 196; break;
    }
}

void App::show_osd(OsdInfo info, double seconds) {
    // Dropped while a panel is open: its own controls already show the value.
    if (panel_open()) return;
    info.until = now_mono() + seconds;
    osd_ = info;
    loop_.cancel(osd_timer_);
    osd_timer_ = loop_.after(seconds + 0.02, [this] {
        osd_timer_ = 0;
        request();
    });
    request();
}

void App::show_toast(const std::string& text) {
    toast_text_ = text;
    toast_until_ = now_mono() + 2.2;
    loop_.after(2.25, [this] { request(); });
    if (!panel_open()) {
        OsdInfo o;
        o.kind = OsdInfo::Message;
        o.value = -1;
        o.glyph = "󰄬";
        o.label = text;
        show_osd(o, 2.0);
    }
    request();
}

// ── geometry ───────────────────────────────────────────────────────────────

float App::shape_radius(float h) const {
    float bh = bar_h();
    return std::min(h / 2.0f, lerp(corner(bh, false), corner(h, true), clamp01((h - bh) / 120.0f)));
}

float App::surface_w() const {
    return island_ && island_->width() > 0 ? static_cast<float>(island_->width()) : static_cast<float>(kSurfaceW);
}

void App::anchor_at(float screen_x) {
    anchor_target_ = screen_x;
    anchored_ = true;
    anchor_pending_ = true;
}

std::string App::bar_signature() const {
    if (bar_layout() != "spread") return "";
    std::string s;
    s.reserve(160);
    for (auto& w : niri_.output_workspaces()) s += std::to_string(w.id) + (w.active ? "a" : "") + (w.urgent ? "u" : "") +
                                                  (w.active_window ? "o" : "") + ",";
    s += std::to_string(static_cast<int>(ws_pill_.value * 100)) + "|";
    if (const auto* w = niri_.focused_window()) s += w->app_id + w->title;
    s += "|";
    for (auto& it : tray_.items()) s += it.key + it.icon_name + it.status + (it.pixmap ? "p" : "");
    const auto& ns = net_.state();
    const auto& as = audio_.state().sink;
    const auto& b = power_.battery();
    bool bt_any = false;
    for (auto& d : bt_.state().devices) bt_any |= d.connected;
    char buf[160];
    snprintf(buf, sizeof(buf), "|%d%d%d%s|%d%d|%d%d|%d%d%d|%d%d|%d", ns.wifi_enabled, ns.ethernet, ns.signal / 34,
             ns.ssid.c_str(), static_cast<int>(as.volume * 100) / 34, as.muted, bt_.state().powered, bt_any, b.percent,
             b.charging, b.plugged, notifs_.dnd_mode(), !notifs_.history().empty(), config_gen_);
    s += buf;
    for (auto& m : config().modules) s += m.id + m.side + (m.on ? "1" : "0");
    return s;
}

// Our own lock screen (ext-session-lock), or the configured command.
void App::lock_screen(double preview) {
    close_all();
    screensaver_.stop();
    const std::string& cmd = config().cmd_lock;
    if (!cmd.empty() && cmd != "internal") {
        proc::spawn_shell(cmd);
        return;
    }
    // the wallpaper (a video's still frame) for the blurred background
    std::string wp = backdrop_.current();
    std::string ext = lower(wp.substr(wp.rfind('.') == std::string::npos ? wp.size() : wp.rfind('.') + 1));
    if (ext == "mp4" || ext == "webm" || ext == "mkv" || ext == "mov") wp = cache_dir() + "/wallpaper-frame.jpg";
    lock_.wallpaper = wp;
    if (!lock_.lock(preview)) proc::spawn_shell("swaylock -f -c 141218");  // no lock protocol: fall back
}

Rect App::island_rect() const {
    float w = std::max(24.0f, w_.value), h = std::max(20.0f, h_.value);
    float sw = surface_w();
    float cx = anchor_x_.value < 0 ? sw / 2 : anchor_x_.value;
    float x = std::round(cx - w / 2);
    x = std::clamp(x, 10.0f, std::max(10.0f, sw - w - 10));
    return {x, std::round(anchor_y_.value), w, h};
}

Rect App::ping_rect() const {
    if (ping_alpha_.value < 0.02f) return {};
    Rect ir = island_rect();
    Rect sr = status_rect();
    float x = sr.empty() ? ir.right() + 8 : sr.right() + 8;
    float ph = bar_h() - 2;
    return {x, 1, ph, ph};
}


// ── input ──────────────────────────────────────────────────────────────────

void App::on_motion(double x, double y) {
    ui_.motion(static_cast<float>(x), static_cast<float>(y));
    Rect ir = island_rect();
    bool over = ir.contains(static_cast<float>(x), static_cast<float>(y));
    // "One island": its sides hold buttons; only the middle opens the summary.
    if (float side = one_island_side(); side > 0 && shown_ == View::Rest)
        over = over && x > ir.x + side && x < ir.right() - side;
    pointer_over_island_ = over;
    bool restish = shown_ == View::Rest || shown_ == View::Glance || shown_ == View::Music;
    if (restish && !panel_open()) {
        if (over && !summary_ && !hover_timer_) {
            hover_timer_ = loop_.after(config().glance_delay_ms / 1000.0, [this] {
                hover_timer_ = 0;
                if (pointer_over_island_ && !panel_open()) {
                    summary_ = true;
                    request();
                }
            });
        } else if (!over) {
            loop_.cancel(hover_timer_);
            hover_timer_ = 0;
            if (summary_ && !ui_.dragging()) {
                summary_ = false;
            }
        }
    }
    request();
}

void App::on_button(Button b, bool pressed) {
    if (ui_.button(b, pressed)) {
        update_keyboard();
        request();
        return;
    }
    if (pressed && island_rect().contains(ui_.px(), ui_.py())) island_click(b);
    update_keyboard();
    request();
}

void App::island_click(Button b) {
    View v = shown_;
    if (b == Button::Left) {
        switch (v) {
            case View::Rest:
            case View::Glance: open(View::Control); break;
            case View::Music:
                if (!niri_.focus_app(mpris_.info().desktop.empty() ? mpris_.info().identity : mpris_.info().desktop))
                    mpris_.raise();
                break;
            case View::Notification:
                if (auto n = notifs_.current()) activate_notification(n);
                break;
            case View::Osd: osd_.until = 0; break;
            default:
                // A click on a panel's empty space ends text entry.
                if (v == View::Calendar && task_input_) task_input_ = false;
                if (v == View::Board) {
                    board_input_ = false;
                    if (board_notes_focus_ && board_detail_ >= 0) store_.set_task_notes(board_detail_, board_notes_.text);
                    board_notes_focus_ = false;
                }
                break;
        }
    } else if (b == Button::Right) {
        if (is_panel(v)) back();
        else if (v == View::Notification) {
            if (auto n = notifs_.current()) notifs_.close(n->id);
        } else {
            open(View::Control);
        }
    } else if (b == Button::Middle) {
        if (mpris_.info().active && !is_panel(v)) mpris_.play_pause();
    }
}

void App::on_scroll(double dx, double dy, bool discrete) {
    (void)dx;
    float steps = discrete ? static_cast<float>(dy) : static_cast<float>(dy) / 12.0f;
    if (ui_.scroll(steps)) {
        request();
        return;
    }
    View v = shown_;
    if (v == View::Rest || v == View::Glance || v == View::Music || v == View::Osd) {
        if (bubble_rect().contains(ui_.px(), ui_.py()) || status_rect().contains(ui_.px(), ui_.py())) return;
        audio_.change_volume(-steps * 0.05f);
    }
    request();
}

void App::paste_into(TextField* f) {
    if (!proc::has("wl-paste")) return;
    auto out = std::make_shared<std::string>();
    loop_.async([out] { *out = proc::run({"wl-paste", "--no-newline", "--type", "text/plain"}, 2000).out; },
                [this, out, f] {
                    f->insert(*out);
                    request();
                });
}

void App::on_key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    request();
    TextField* field = nullptr;
    switch (shown_) {
        case View::Launcher: field = &query_; break;
        case View::WifiPassword: field = &pass_field_; break;
        case View::NoteEditor: field = note_focus_ == 0 ? &note_title_ : &note_body_; break;
        case View::Clipboard: field = &clip_query_; break;
        case View::Calendar:
            if (task_input_) field = &task_field_;
            break;
        case View::Board:
            if (board_input_) field = &board_field_;
            else if (board_notes_focus_) field = &board_notes_;
            break;
        default: break;
    }
    if (!field && shown_ == View::Session) {
        if (sym == XKB_KEY_Left) session_sel_ = (session_sel_ + 4) % 5;
        else if (sym == XKB_KEY_Right || sym == XKB_KEY_Tab) session_sel_ = (session_sel_ + 1) % 5;
        else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space) session_activate(session_sel_);
        else if (sym == XKB_KEY_Escape) back();
        update_keyboard();
        return;
    }
    if (!field) {
        if (sym == XKB_KEY_Escape && shown_ == View::Timer && tm_mode_ != 0) timer_mode(0, {});
        else if (sym == XKB_KEY_Escape) back();
        update_keyboard();
        return;
    }
    auto r = field->key(sym, utf8, mods);
    if (r == TextField::Result::Paste) {
        paste_into(field);
        return;
    }
    switch (shown_) {
        case View::Launcher:
            if (r == TextField::Result::Changed) {
                sel_ = 0;
                launcher_scroll_.set(0);
                refresh_launcher();
            } else if (r == TextField::Result::Down || (r == TextField::Result::Tab && !(mods & Wayland::MOD_SHIFT))) {
                if (!results_.empty()) sel_ = (sel_ + 1) % static_cast<int>(results_.size());
            } else if (r == TextField::Result::Up || r == TextField::Result::Tab) {
                if (!results_.empty()) sel_ = (sel_ - 1 + static_cast<int>(results_.size())) % static_cast<int>(results_.size());
            } else if (r == TextField::Result::Submit) {
                launcher_activate(sel_);
            } else if (r == TextField::Result::Cancel) {
                if (!query_.text.empty()) {
                    query_.clear();
                    refresh_launcher();
                } else {
                    back();
                }
            }
            // keep the selection in view
            {
                float row = 50, view = 8 * row;
                float top = sel_ * row;
                if (top < launcher_scroll_.target) launcher_scroll_.set(top);
                if (top + row > launcher_scroll_.target + view) launcher_scroll_.set(top + row - view);
            }
            break;
        case View::Clipboard:
            if (r == TextField::Result::Cancel) {
                if (!clip_query_.text.empty()) clip_query_.clear();
                else back();
            } else if (r == TextField::Result::Changed) {
                clip_list_scroll_.reset(0);
                clip_sel_id_.clear();
            } else if (r == TextField::Result::Up || r == TextField::Result::Down || r == TextField::Result::Submit) {
                clip_key_nav_ = r == TextField::Result::Up ? -1 : r == TextField::Result::Down ? 1 : 2;
            }
            break;
        case View::WifiPassword:
            if (r == TextField::Result::Submit && pass_field_.text.size() >= 8) net_.connect(pass_ssid_, pass_field_.text);
            else if (r == TextField::Result::Cancel) back();
            break;
        case View::NoteEditor:
            if (r == TextField::Result::Cancel) {
                back();
            } else if (note_focus_ == 0 && (r == TextField::Result::Submit || r == TextField::Result::Down ||
                                            r == TextField::Result::Tab)) {
                note_focus_ = 1;
            } else if (note_focus_ == 1 && r == TextField::Result::Up && note_body_.caret == 0) {
                note_focus_ = 0;
            } else if (r == TextField::Result::Changed) {
                note_dirty_ = true;
                loop_.cancel(note_save_timer_);
                note_save_timer_ = loop_.after(0.5, [this] {
                    note_save_timer_ = 0;
                    flush_note();
                });
            }
            break;
        case View::Calendar:
            if (r == TextField::Result::Submit) submit_task_input();
            else if (r == TextField::Result::Cancel) task_input_ = false;
            break;
        case View::Board:
            if (board_input_) {
                if (r == TextField::Result::Submit) submit_board_input();
                else if (r == TextField::Result::Cancel) board_input_ = false;
            } else if (board_notes_focus_) {
                if (r == TextField::Result::Cancel || (r == TextField::Result::Submit)) {
                    store_.set_task_notes(board_detail_, board_notes_.text);
                    board_notes_focus_ = false;
                }
            }
            break;
        default: break;
    }
    update_keyboard();
}

// ── frame ──────────────────────────────────────────────────────────────────

void App::render() {
    static const bool debug = getenv("NIRI_ISLAND_DEBUG") != nullptr;
    static int frames = 0;
    static double busy = 0, window = now_mono(), worst = 0;
    double t_start = now_mono();
    struct Report {
        double& t0;
        ~Report() {
            if (!debug) return;
            double took = now_mono() - t0;
            busy += took;
            worst = std::max(worst, took);
            ++frames;
            if (now_mono() - window > 5.0) {
                fprintf(stderr, "[debug] %.1f fps, %.2f ms/frame, worst %.2f ms\n", frames / (now_mono() - window),
                        frames ? busy * 1000.0 / frames : 0.0, worst * 1000.0);
                frames = 0;
                busy = 0;
                worst = 0;
                window = now_mono();
            }
        }
    } report{t_start};
    double now = now_mono();
    float dt = last_frame_ > 0 ? static_cast<float>(std::clamp(now - last_frame_, 0.0, 0.05)) : 0.016f;
    last_frame_ = now;
    anim_time_ += dt;

    ui_.begin_frame(dt);
    bool anim = ui_.animating();
    anim |= Theme::get().update(dt);

    View v = decide();
    if (v != shown_) {
        // Deck <-> paper has its own transition (the card grows / shrinks).
        bool paper_flip = (v == View::NoteEditor && shown_ == View::Notes) || (v == View::Notes && shown_ == View::NoteEditor);
        if (paper_flip && v == View::Notes) {
            note_return_id_ = note_id_;
            note_collapse_.reset(1.0f);
            note_collapse_.set(0.0f);
        }
        on_view_leave(shown_);
        // Freeze what was on screen: the outgoing content is drawn once into
        // a snapshot and only faded/scaled from then on.
        if (snap_prev_) cairo_surface_destroy(snap_prev_);
        if (snap_next_ && snap_next_view_ == shown_ && snap_next_w_ == target_w_ && snap_next_h_ == target_h_) {
            snap_prev_ = snap_next_;
            snap_next_ = nullptr;
        } else {
            snap_prev_ = Spring::speed > 0 ? snapshot(shown_, target_w_, target_h_) : nullptr;
        }
        if (snap_next_) {
            cairo_surface_destroy(snap_next_);
            snap_next_ = nullptr;
        }
        prev_ = shown_;
        prev_w_ = target_w_;
        prev_h_ = target_h_;
        shown_ = v;
        morph_ = Spring::speed > 0 && !paper_flip ? 0.0f : 1.0f;
        on_view_enter(v);
        ui_.cancel();
        update_keyboard();
        if (v != View::Osd) osd_pinned_ = false;
        wl_.set_shield(panel_open() || osd_pinned_);
    }
    if (auto n = notifs_.current(); n && n->id != notif_shown_id_) {
        notif_shown_id_ = n->id;
        notif_shown_at_ = now_mono();
        notif_expanded_ = false;
    }
    sizes_for(shown_, target_w_, target_h_);
    w_.set(target_w_);
    h_.set(target_h_);
    anim |= w_.update(dt);
    anim |= h_.update(dt);
    if (morph_ < 1.0f) {
        morph_ = std::min(1.0f, morph_ + dt * Spring::speed / 0.30f);
        anim = true;
    }

    // Workspaces bubble beside the resting island.
    bool grouped = bar_layout() == "grouped";
    bool summary_view = shown_ == View::Rest || shown_ == View::Glance || shown_ == View::Music;
    bool want_left = grouped && summary_view && !side_caps(true).empty();
    st_alpha_.set(grouped && summary_view && !side_caps(false).empty() ? 1.0f : 0.0f);
    anim |= st_alpha_.update(dt);
    ws_alpha_.set(want_left ? 1.0f : 0.0f);
    ws_pill_.set(static_cast<float>(niri_.active_slot()));
    anim |= ws_alpha_.update(dt);
    anim |= ping_alpha_.update(dt);
    {
        // Back to the middle once whatever was opened from a corner is gone.
        float sw = surface_w();
        if (anchored_ && !panel_open() && shown_ != View::Osd) anchored_ = false;
        float target = anchored_ ? anchor_target_ : sw / 2;
        if (anchor_x_.value < 0) anchor_x_.reset(target);
        anchor_x_.set(target);
        anim |= anchor_x_.update(dt);
        // Below the strip (32 px + a gap), like a menu dropping from its button.
        anchor_y_.set(anchored_ ? bar_h() + 8 : 0.0f);
        anim |= anchor_y_.update(dt);
    }
    anim |= ws_pill_.update(dt);

    // Scroll springs
    for (Spring* s : {&inbox_scroll_, &clip_list_scroll_, &clip_text_scroll_, &clip_scroll_, &tm_anim_, &snd_tab_x_, &snd_scroll_, &cc_notif_scroll_, &wifi_scroll_, &bt_scroll_, &task_scroll_, &notes_scroll_, &launcher_scroll_,
                      &note_expand_, &note_collapse_})
        anim |= s->update(dt);

    Rect ir = island_rect();
    Rect bbox = ir.inset(-26, -4);
    bbox.h += 26;
    if (Rect brr = bubble_rect(); !brr.empty()) bbox = bbox.unite(brr.inset(-24, -4));
    if (Rect pr = ping_rect(); !pr.empty()) bbox = bbox.unite(pr.inset(-20, -4));
    if (Rect sr = status_rect(); !sr.empty()) bbox = bbox.unite(sr.inset(-24, -4));
    cairo_t* cr = island_ ? island_->begin_frame(bbox) : nullptr;
    if (!cr) {
        ui_.abort_frame();
        dirty_ = true;
        return;
    }
    dirty_ = false;
    if (island_hidden_) {
        // Out of the way while a screenshot is taken.
        if (!last_input_.empty()) {
            island_->set_input_rects({});
            last_input_.clear();
        }
        island_->end_frame(bbox.unite(last_bbox_));
        last_bbox_ = bbox;
        return;
    }

    float bh = bar_h();
    float radius = shape_radius(ir.h);
    bool big = ir.h > bh + 28;

    bool docked = attached() && ir.y < 0.5f;  // a panel dropped below a corner strip floats
    island_shadow(cr, ir, radius, docked, big ? 1.0f : 0.6f);
    Color body = glass_body();
    if (shown_ == View::NoteEditor) {
        // The island becomes the note's paper once the card has grown into it.
        static const uint32_t papers[] = {0xFEF6E4, 0xD8E2F3, 0xE4F5E1, 0xE3EEFF, 0xFBE3EA};
        Color paper = Color::hex(0xFEF6E4);
        if (Note* n = store_.note(note_id_)) paper = Color::hex(papers[std::clamp(n->color, 0, 4)]);
        body = mix(body, paper, smoothstep((note_expand_.value - 0.9f) / 0.1f));
    }
    island_outline(cr, ir, radius, docked);
    set(cr, body);
    cairo_fill(cr);

    // A rounded clip makes cairo mask every operation. Content never reaches
    // the corners once the island is still, so the cheap rectangular clip
    // does then; the rounded one is only needed while the shape morphs.
    bool morphing = morph_ < 1.0f || !w_.settled() || !h_.settled();
    cairo_save(cr);
    if (morphing) island_outline(cr, ir, radius, docked);
    else cairo_rectangle(cr, ir.x, ir.y, ir.w, ir.h);
    cairo_clip(cr);

    // Content is revealed by the island's clip, never resampled: pixel-aligned
    // blits are plain copies, where scaled ones cost milliseconds each.
    auto place = [&](float tw, float, float& ox, float& oy, float& sc) {
        sc = 1.0f;
        ox = std::round(ir.x + (ir.w - tw) / 2.0f);
        // Anchored to the top edge: centring it vertically made text slide
        // through the island's edge (and get cut) while the height animates.
        oy = std::round(ir.y);
    };
    auto blit = [&](cairo_surface_t* snap, float tw, float th, float alpha, float dy) {
        if (!snap || alpha <= 0.01f) return;
        float ox, oy, sc;
        place(tw, th, ox, oy, sc);
        (void)sc;
        cairo_set_source_surface(cr, snap, ox, std::round(oy + dy));
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
        cairo_paint_with_alpha(cr, alpha);
    };

    if (morph_ < 1.0f) {
        // Cross-fade between two snapshots: two blits per frame instead of
        // drawing both views (and grouping them) sixty times a second.
        blit(snap_prev_, prev_w_, prev_h_, 1.0f - smoothstep(morph_ / 0.42f), 0);
        if (!snap_next_ || snap_next_view_ != shown_ || snap_next_w_ != target_w_ || snap_next_h_ != target_h_) {
            if (snap_next_) cairo_surface_destroy(snap_next_);
            snap_next_ = snapshot(shown_, target_w_, target_h_);
            snap_next_view_ = shown_;
            snap_next_w_ = target_w_;
            snap_next_h_ = target_h_;
        }
        float a_now = smoothstep((morph_ - 0.12f) / 0.55f);
        blit(snap_next_, target_w_, target_h_, a_now, (1.0f - a_now) * 5.0f);
        // Keep the incoming view clickable while it settles: run its drawing
        // against a fully clipped context, which only registers its regions.
        if (a_now > 0.4f) {
            static cairo_surface_t* null_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
            cairo_t* ncr = cairo_create(null_surface);
            cairo_rectangle(ncr, 0, 0, 0, 0);
            cairo_clip(ncr);
            float ox, oy, sc;
            place(target_w_, target_h_, ox, oy, sc);
            ui_.push(ox, oy, sc);
            draw_view(ncr, shown_, target_w_, target_h_);
            ui_.pop();
            cairo_destroy(ncr);
        }
    } else {
        if (snap_prev_) {
            cairo_surface_destroy(snap_prev_);
            snap_prev_ = nullptr;
        }
        if (snap_next_) {
            cairo_surface_destroy(snap_next_);
            snap_next_ = nullptr;
        }
        float ox, oy, sc;
        place(target_w_, target_h_, ox, oy, sc);
        cairo_save(cr);
        cairo_translate(cr, ox, oy);
        cairo_scale(cr, sc, sc);
        ui_.push(ox, oy, sc);
        draw_view(cr, shown_, target_w_, target_h_);
        ui_.pop();
        cairo_restore(cr);
    }
    cairo_restore(cr);


    draw_side(cr, true);
    draw_side(cr, false);
    draw_ping(cr);

    // Input follows the island exactly; the rest of the surface lets clicks through.
    std::vector<Rect> input{ir};
    if (ws_alpha_.value > 0.5f)
        for (auto& r : side_rects(true)) input.push_back(r);
    if (Rect pr = ping_rect(); !pr.empty() && ping_alpha_.value > 0.5f) input.push_back(pr);
    if (st_alpha_.value > 0.5f)
        for (auto& r : side_rects(false)) input.push_back(r);
    bool changed = input.size() != last_input_.size();
    for (size_t i = 0; !changed && i < input.size(); ++i)
        changed = std::abs(input[i].x - last_input_[i].x) > 0.5f || std::abs(input[i].w - last_input_[i].w) > 0.5f ||
                  std::abs(input[i].h - last_input_[i].h) > 0.5f;
    if (changed) {
        island_->set_input_rects(input);
        last_input_ = input;
    }

    // Compositor blur behind the island and its capsules.
    if (config().glass && config().compositor_blur && wl_.can_blur()) {
        std::vector<LayerSurface::BlurShape> blur{{ir, radius, docked}};
        for (bool left : {true, false})
            if ((left ? ws_alpha_ : st_alpha_).value > 0.5f)
                for (auto& r : side_rects(left)) blur.push_back({r, corner(r.h, false), sides_docked()});
        if (Rect r = ping_rect(); !r.empty() && ping_alpha_.value > 0.5f) blur.push_back({r, 15, false});
        island_->set_blur(blur);
    } else {
        island_->set_blur({});
    }

    Rect damage = bbox.unite(last_bbox_);
    last_bbox_ = bbox;
    island_->end_frame(damage);
    island_->set_cursor(ui_.cursor());

    // What keeps moving without springs: spectrum bars, marquees, progress.
    bool playing = mpris_.info().playing;
    bool ambient = recorder_.active() || (playing && (shown_ == View::Rest || shown_ == View::Music || shown_ == View::Control)) ||
                   (shown_ == View::Wifi && net_.state().scanning) || (shown_ == View::Bluetooth && bt_.state().discovering) ||
                   (shown_ == View::Timer && clock_active()) || shown_ == View::Glance ||
                   ((shown_ == View::Rest || shown_ == View::Music) && stopwatch_.running() && !stopwatch_.paused()) ||
                   (shown_ == View::Notification && notifs_.current() && notifs_.current()->timeout_ms != 0);
    cava_.set_running(playing && (shown_ == View::Rest || shown_ == View::Music));

    if (anim || ui_.dragging() || morph_ < 1.0f) {
        request();
    } else if (ambient && !low_rate_timer_) {
        // 30 fps is plenty for ambient motion and halves the cost.
        low_rate_timer_ = loop_.after(1.0 / 30.0, [this] {
            low_rate_timer_ = 0;
            request();
        });
    }
}

cairo_surface_t* App::snapshot(View v, float w, float h) {
    int sc = scale();
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(std::ceil(w * sc)),
                                                       static_cast<int>(std::ceil(h * sc)));
    cairo_surface_set_device_scale(surf, sc, sc);
    cairo_t* scr = cairo_create(surf);
    bool was = ui_.enabled();
    ui_.set_enabled(false);
    draw_view(scr, v, w, h);
    ui_.set_enabled(was);
    cairo_destroy(scr);
    return surf;
}

// Three box blurs ≈ a gaussian: soft, round shadows instead of stacked rings.
static void box_blur_a8(unsigned char* px, int w, int h, int stride, int radius) {
    if (radius < 1) return;
    std::vector<unsigned char> tmp(static_cast<size_t>(std::max(w, h)));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {  // horizontal
            unsigned char* row = px + y * stride;
            int sum = 0, n = 2 * radius + 1;
            for (int k = -radius; k <= radius; ++k) sum += row[std::clamp(k, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                tmp[x] = static_cast<unsigned char>(sum / n);
                sum += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
            }
            std::copy(tmp.begin(), tmp.begin() + w, row);
        }
        for (int x = 0; x < w; ++x) {  // vertical
            int sum = 0, n = 2 * radius + 1;
            for (int k = -radius; k <= radius; ++k) sum += px[std::clamp(k, 0, h - 1) * stride + x];
            for (int y = 0; y < h; ++y) {
                tmp[y] = static_cast<unsigned char>(sum / n);
                sum += px[std::min(y + radius + 1, h - 1) * stride + x] - px[std::max(y - radius, 0) * stride + x];
            }
            for (int y = 0; y < h; ++y) px[y * stride + x] = tmp[y];
        }
    }
}

// A blurred rounded rect (w×h at `pad` inside a (w+2pad)×(h+2pad) picture).
static cairo_surface_t* raster_shadow(float w, float h, float pad, float radius, float alpha, float spread, int sc) {
    int pw = static_cast<int>(std::ceil((w + 2 * pad) * sc)), ph = static_cast<int>(std::ceil((h + 2 * pad) * sc));
    cairo_surface_t* mask = cairo_image_surface_create(CAIRO_FORMAT_A8, pw, ph);
    cairo_t* m = cairo_create(mask);
    cairo_scale(m, sc, sc);
    paint::rrect(m, {pad, pad, w, h}, radius);
    cairo_set_source_rgba(m, 0, 0, 0, 1);
    cairo_fill(m);
    cairo_destroy(m);
    cairo_surface_flush(mask);
    box_blur_a8(cairo_image_surface_get_data(mask), pw, ph, cairo_image_surface_get_stride(mask),
                std::max(1, static_cast<int>(std::lround(spread * sc / 2.2f))));
    cairo_surface_mark_dirty(mask);
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    cairo_t* c = cairo_create(out);
    cairo_set_source_rgba(c, 0, 0, 0, alpha);
    cairo_mask_surface(c, mask, 0, 0);
    cairo_destroy(c);
    cairo_surface_destroy(mask);
    cairo_surface_set_device_scale(out, sc, sc);
    return out;
}

void App::draw_shadow(cairo_t* cr, const Rect& r, float radius, float alpha, float spread) {
    // Big shapes: nine-slice of a blurred square large enough for its middle
    // to reach full strength, edges stretched. Small ones (pills, docked
    // shapes): rasterised at their exact size, cached (their size rarely changes).
    int sc = scale();
    float pad = std::ceil(spread * 1.6f) + 2;
    float inner = std::ceil(radius + spread * 1.2f);
    float cut = pad + inner;
    float mid_w = r.w + 2 * pad - 2 * cut, mid_h = r.h + 2 * pad - 2 * cut;
    std::string base = std::to_string(static_cast<int>(radius)) + "a" + std::to_string(static_cast<int>(alpha * 100)) + "s" +
                       std::to_string(static_cast<int>(spread)) + "@" + std::to_string(sc);
    if (shadow_cache_.size() > 32) {
        for (auto& [k, v] : shadow_cache_)
            if (v) cairo_surface_destroy(v);
        shadow_cache_.clear();
    }
    if (mid_w < 0 || mid_h < 0) {
        float w = std::round(r.w), h = std::round(r.h);
        cairo_surface_t*& img = shadow_cache_[base + "x" + std::to_string(static_cast<int>(w)) + "y" + std::to_string(static_cast<int>(h))];
        if (!img) img = raster_shadow(w, h, pad, radius, alpha, spread, sc);
        cairo_set_source_surface(cr, img, std::round(r.x) - pad, std::round(r.y) - pad);
        cairo_paint(cr);
        return;
    }
    float b = 2 * inner + 2;
    int size = static_cast<int>(2 * cut + 2);
    cairo_surface_t*& src = shadow_cache_[base + "n"];
    if (!src) src = raster_shadow(b, b, pad, radius, alpha, spread, sc);
    float X = std::round(r.x) - pad, Y = std::round(r.y) - pad;
    float c = cut;
    float sx[3] = {0, c, c + 2}, sw[3] = {c, 2, c};
    float dx[3] = {X, X + c, X + c + mid_w}, dw[3] = {c, mid_w, c};
    float dy[3] = {Y, Y + c, Y + c + mid_h}, dh[3] = {c, mid_h, c};
    (void)size;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            if (dw[i] <= 0 || dh[j] <= 0) continue;
            if (i == 1 && j == 1) continue;  // under the shape: clipped away anyway
            cairo_save(cr);
            cairo_rectangle(cr, dx[i], dy[j], dw[i], dh[j]);
            cairo_clip(cr);
            cairo_translate(cr, dx[i], dy[j]);
            cairo_scale(cr, dw[i] / sw[i], dh[j] / sw[j]);
            cairo_translate(cr, -sx[i], -sx[j]);
            cairo_set_source_surface(cr, src, 0, 0);
            cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }
    }
}

void App::draw_view(cairo_t* cr, View v, float w, float h) {
    switch (v) {
        case View::Rest: draw_rest(cr, w, h); break;
        case View::Glance: draw_glance(cr, w, h); break;
        case View::Music: draw_music(cr, w, h); break;
        case View::Notification: draw_notification(cr, w, h); break;
        case View::Osd: draw_osd(cr, w, h); break;
        case View::Control: draw_control(cr, w, h); break;
        case View::Wifi: draw_wifi(cr, w, h); break;
        case View::WifiPassword: draw_wifi_password(cr, w, h); break;
        case View::WifiInfo: draw_wifi_info(cr, w, h); break;
        case View::Bluetooth: draw_bluetooth(cr, w, h); break;
        case View::Sound: draw_sound(cr, w, h); break;
        case View::Clipboard: draw_clipboard(cr, w, h); break;
        case View::Inbox: draw_inbox(cr, w, h); break;
        case View::Tray: draw_tray(cr, w, h); break;
        case View::Weather: draw_weather(cr, w, h); break;
        case View::Calendar: draw_calendar(cr, w, h); break;
        case View::DatePicker: draw_picker(cr, w, h); break;
        case View::Board: draw_board(cr, w, h); break;
        case View::Notes: draw_notes(cr, w, h); break;
        case View::NoteEditor: draw_note_editor(cr, w, h); break;
        case View::Launcher: draw_launcher(cr, w, h); break;
        case View::Session: draw_session(cr, w, h); break;
        case View::Stats: draw_stats(cr, w, h); break;
        case View::Timer: draw_timer(cr, w, h); break;
        case View::Capture: draw_capture(cr, w, h); break;
        case View::Record: draw_record(cr, w, h); break;
    }
}

}  // namespace island
