// The control centre: quick tiles and levels, the player, weather and month,
// and the notification history.

#include <climits>
#include <dirent.h>
#include <nlohmann/json.hpp>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;
using json = nlohmann::json;

static const char* kMonths[] = {"Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
                                "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"};

static void month_of(int offset, int& y, int& m0) {
    struct tm now = local_tm();
    int raw = now.tm_mon + offset;
    y = now.tm_year + 1900 + (raw >= 0 ? raw / 12 : (raw - 11) / 12);
    m0 = ((raw % 12) + 12) % 12;
}

void App::draw_month(cairo_t* cr, const Rect& r, int offset, int selected_day, bool interactive, const char* idbase,
                     std::function<void(int, int, int)> on_pick, std::function<void(int)> on_nav, int mark_y,
                     int mark_m, int mark_d) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    int y, m0;
    month_of(offset, y, m0);
    struct tm now = local_tm();
    bool this_month = y == now.tm_year + 1900 && m0 == now.tm_mon;

    std::string title = std::string(kMonths[m0]) + " " + std::to_string(y);
    text(cr, r.x + 4, r.y + 2, title, {ui(14, "Bold"), p.text});
    if (on_nav) {
        w::icon_button(c, ID(idbase, 1), {r.right() - 60, r.y - 3, 28, 28}, "󰅁", 14).click([on_nav] { on_nav(-1); });
        w::icon_button(c, ID(idbase, 2), {r.right() - 30, r.y - 3, 28, 28}, "󰅂", 14).click([on_nav] { on_nav(1); });
        if (offset != 0)
            w::pill(c, ID(idbase, 3), {r.right() - 112, r.y - 1, 48, 24}, "Hoy", nullptr, w::Style::Tonal, 11.5f)
                .click([on_nav, offset] { on_nav(-offset); });
    }

    static const char* wd[] = {"L", "M", "X", "J", "V", "S", "D"};
    float gy = r.y + 32;
    float cw = r.w / 7.0f;
    for (int i = 0; i < 7; ++i)
        text(cr, r.x + cw * i + cw / 2, gy, wd[i], {ui(11, "SemiBold"), i >= 5 ? p.text_faint : p.text_dim, Align::Center});

    int first = first_weekday_monday(y, m0);
    int days = days_in_month(y, m0);
    float rows_h = r.bottom() - (gy + 20);
    float rh = std::min(34.0f, rows_h / 6.0f);
    auto marks = store_.task_days(y, m0);
    for (int d = 1; d <= days; ++d) {
        int cell = first + d - 1;
        int row = cell / 7, col = cell % 7;
        Rect cellr{r.x + col * cw, gy + 20 + row * rh, cw, rh};
        float sz = std::min(cellr.w, cellr.h) - 4;
        Rect dot{cellr.cx() - sz / 2, cellr.cy() - sz / 2, sz, sz};
        bool today = this_month && d == now.tm_mday;
        bool sel = d == selected_day;
        bool marked = mark_y == y && mark_m == m0 && mark_d == d;
        uint64_t id = ID(idbase, 100 + d);
        float hv = interactive ? ui_.hover(id) : 0;
        if (today) fill(cr, dot, sz / 2, p.accent);
        else if (sel || marked) fill(cr, dot, sz / 2, p.accent_dim);
        else if (hv > 0.01f) fill(cr, dot, sz / 2, p.text.with_alpha(0.08f * hv));
        if (marked && !today) stroke(cr, dot, sz / 2, p.accent, 1.5f);
        Color tc = today ? p.on_accent : col >= 5 ? p.text_dim : p.text;
        text(cr, cellr.cx(), cellr.cy() - 8, std::to_string(d), {ui(12.5f, today || sel ? "Bold" : ""), tc, Align::Center});
        if (std::find(marks.begin(), marks.end(), d) != marks.end())
            fill(cr, {cellr.cx() - 2, dot.bottom() - 4, 4, 4}, 2, today ? p.on_accent : p.warn);
        if (interactive && on_pick) ui_.add(id, cellr).click([on_pick, y, m0, d] { on_pick(y, m0, d); });
    }
}

static std::string wallpaper_path() {
    std::string text;
    if (!read_file(expand_home(config().wallpaper_file), text)) return "";
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return "";
    std::string path = j.value("thumb", j.value("path", ""));
    return path;
}

void App::draw_control(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    (void)h;

    // ── top bar: system on the left, tools on the right, the date between ──
    float ty = 12;
    struct TopBtn {
        const char* id;
        const char* glyph;
        std::function<void()> fn;
        bool on = false;
    };
    TopBtn left[] = {
        {"cc.session", "󰐥", [this] { open(View::Session); }},
        {"cc.lock", "󰌾",
         [this] {
             close_all();
             lock_screen();
         }},
        {"cc.night", "󰖔", [] { proc::spawn_shell(config().cmd_night_light); }},
        {"cc.gaming", "󰊴", [this] { set_gaming_mode(!gaming_mode_); }, gaming_mode_},
        {"cc.settings", "󰒓", [this] { open_settings(); }},
        {"cc.capture", "󰄀", [this] {
             // the panel goes first: it is not wanted in the frozen screen
             close_all();
             loop_.after(0.35, [this] { start_capture(0); });
         }},
        {"cc.stats", "󰓅", [this] { open(View::Stats); }},
    };
    TopBtn right[] = {
        {"cc.launcher", "󰍉", [this] { open(View::Launcher); }},
        {"cc.clip", "󰅌", [this] { open(View::Clipboard); }},
        {"cc.notes", "󰠮", [this] { open(View::Notes); }},
        {"cc.board", "󰄲", [this] { open(View::Board); }},
        {"cc.timer", "󱎫", [this] { open(View::Timer); }, clock_active()},
        {"cc.sound", "󰕾", [this] { open(View::Sound); }},
    };
    float lx = 14;
    for (auto& b : left) {
        w::icon_button(c, ID(b.id), {lx, ty, 32, 32}, b.glyph, 15, b.on).click(b.fn);
        lx += 34;
    }
    float rx = w - 14 - 32;
    w::icon_button(c, ID("cc.close"), {rx, ty, 32, 32}, "󰅖", 15).click([this] { close_all(); });
    divider(cr, rx - 5, ty + 8, rx - 5, ty + 24, p.border);
    rx -= 42;
    for (int i = 5; i >= 0; --i) {
        w::icon_button(c, ID(right[i].id), {rx, ty, 32, 32}, right[i].glyph, 15, right[i].on).click(right[i].fn);
        rx -= 34;
    }
    std::string date = fmt_time("%A %e de %B");
    if (!date.empty()) date[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(date[0])));
    // Centred on the panel, in the room both groups leave (a width makes
    // Align::Center centre inside [x, x + width]).
    float room = 2 * std::min(w / 2 - lx, rx + 32 - w / 2) - 24;
    text(cr, w / 2 - room / 2, ty + 7, date, {ui(13.5f, "SemiBold"), p.text_dim, Align::Center, room});

    // ── column 1: tiles, levels, appearance ──────────────────────────
    float c1 = 16, c1w = 282, y0 = 56;
    const auto& ns = net_.state();
    const auto& bs = bt_.state();
    std::string bt_sub = !bs.available ? "No disponible" : !bs.powered ? "Apagado" : "Sin conectar";
    for (auto& d : bs.devices)
        if (d.connected) {
            bt_sub = d.name + (d.battery >= 0 ? " · " + std::to_string(d.battery) + "%" : "");
            break;
        }
    std::string wifi_sub = !ns.wifi_enabled ? "Apagado" : !ns.ssid.empty() ? (demo_ ? "Mi Wi-Fi" : ns.ssid) : ns.ethernet ? "Ethernet" : "Sin conexión";
    std::string pp = power_.profile();
    const char* pp_glyph = gaming_mode_ ? "󰊴" : pp == "performance" ? "󰓅" : pp == "power-saver" ? "󰾆" : "󰾅";
    std::string pp_sub = gaming_mode_ ? "Gaming Pro" : pp == "performance" ? "Rendimiento" : pp == "power-saver" ? "Ahorro" : "Equilibrado";
    bool mic_on = !audio_.state().source.muted;

    struct Tile {
        const char* id;
        const char* glyph;
        std::string title, sub;
        bool active;
        std::function<void()> click, open;
    };
    Tile tiles[6] = {
        {"tile.wifi", ns.wifi_enabled ? (ns.ssid.empty() ? "󰤯" : ns.signal > 66 ? "󰤨" : ns.signal > 33 ? "󰤥" : "󰤟") : "󰤭",
         "Wi-Fi", wifi_sub, ns.wifi_enabled, [this] { net_.set_wifi(!net_.state().wifi_enabled); },
         [this] { open(View::Wifi); }},
        {"tile.bt", bs.powered ? "󰂯" : "󰂲", "Bluetooth", bt_sub, bs.powered,
         [this] { bt_.set_powered(!bt_.state().powered); }, [this] { open(View::Bluetooth); }},
        {"tile.power", pp_glyph, gaming_mode_ ? "Modo Gaming" : "Energía", pp_sub, gaming_mode_ || pp != "balanced",
         [this] {
             if (gaming_mode_) set_gaming_mode(false);
             else power_.cycle_profile();
         },
         [this] { set_gaming_mode(!gaming_mode_); }},
        {"tile.dnd", notifs_.dnd_mode() == Notifications::DndTotal ? "󰂛" : notifs_.dnd() ? "󱏧" : "󰂚", "No molestar",
         Notifications::dnd_label(notifs_.dnd_mode()), notifs_.dnd(), [this] { notifs_.cycle_dnd(); }, {}},
        {"tile.mic", mic_on ? "󰍬" : "󰍭", "Micrófono", mic_on ? (audio_.mic_in_use() ? "En uso" : "Activo") : "Silenciado",
         mic_on, [this] { audio_.toggle_mic_mute(); }, [this] { open(View::Sound); }},
        {"tile.air", "󰀝", "Modo avión", airplane() ? "Activado" : "Desactivado", airplane(),
         [this] { set_airplane(!airplane()); }, {}},
    };
    float tw = (c1w - 8) / 2, th = 52;
    for (int i = 0; i < 6; ++i) {
        Tile& t = tiles[i];
        Rect r{c1 + (i % 2) * (tw + 8), y0 + (i / 2) * (th + 8), tw, th};
        uint64_t id = ID(t.id);
        float hv = ui_.hover(id);
        float s = w::press_scale(ui_, id, 0.05f);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s);
        fill(cr, r, 16, t.active ? mix(p.accent_dim, p.accent, 0.08f * hv) : mix(p.surface, p.surface_hi, hv));
        if (t.active) stroke(cr, r, 16, p.accent.with_alpha(0.25f + 0.2f * hv), 1);
        Rect badge{r.x + 9, r.cy() - 14, 28, 28};
        fill(cr, badge, 14, t.active ? p.accent : p.surface_hi);
        glyph(cr, badge.cx(), badge.cy(), t.glyph, 14, t.active ? p.on_accent : p.text_dim);
        float sx = badge.right() + 8, sw = r.right() - sx - (t.open ? 18 : 8);
        text(cr, sx, r.y + 9, t.title, {ui(12.5f, "SemiBold"), p.text, Align::Left, sw});
        text(cr, sx, r.y + 27, t.sub, {ui(11), t.active ? mix(p.accent, p.text, 0.25f) : p.text_dim, Align::Left, sw});
        cairo_restore(cr);
        auto& reg = ui_.add(id, r).click(t.click);
        if (t.open) {
            reg.right(t.open);
            Rect chev{r.right() - 20, r.y, 20, r.h};
            uint64_t cid = ID(t.id, 1);
            glyph(cr, chev.cx() - 2, chev.cy(), "󰅂", 12, mix(p.text_faint, p.text, ui_.hover(cid)));
            ui_.add(cid, chev).click(t.open);
        }
    }

    float sy = y0 + 3 * (th + 8) + 4;
    const auto& sink = audio_.state().sink;
    w::slider(c, ID("cc.vol"), {c1, sy, c1w, 40}, std::min(1.0f, sink.volume),
              sink.muted || sink.volume < 0.01f ? "󰝟" : "󰕾", [this](float v) { audio_.set_volume(v); },
              [this] { audio_.toggle_mute(); }, "", sink.muted);
    if (power_.has_backlight()) {
        w::slider(c, ID("cc.bright"), {c1, sy + 48, c1w, 40}, power_.brightness(), "󰃠",
                  [this](float v) { power_.set_brightness(v); });
    } else {
        w::slider(c, ID("cc.micvol"), {c1, sy + 48, c1w, 40}, std::min(1.0f, audio_.state().source.volume),
                  audio_.state().source.muted ? "󰍭" : "󰍬", [this](float v) { audio_.set_mic_volume(v); },
                  [this] { audio_.toggle_mic_mute(); }, "", audio_.state().source.muted);
    }

    // Appearance: the wallpaper and the palette taken from it.
    Rect app{c1, sy + 100, c1w, h - (sy + 100) - 16};
    {
        uint64_t id = ID("cc.appearance");
        float hv = ui_.hover(id);
        std::string wp = wallpaper_path();
        cairo_surface_t* thumb = wp.empty() ? nullptr : ImageCache::get().request(wp, 320);
        fill(cr, app, 18, p.surface);
        if (thumb) image(cr, thumb, app, 18, 0.92f + 0.08f * hv);
        cairo_save(cr);
        rrect(cr, app, 18);
        cairo_clip(cr);
        cairo_pattern_t* g = cairo_pattern_create_linear(0, app.y + app.h * 0.35, 0, app.bottom());
        cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0);
        cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0.82);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
        cairo_restore(cr);
        Color sw[] = {p.accent, p.info, p.accent_dim, p.text};
        for (int i = 0; i < 4; ++i) {
            Rect s{app.x + 14 + (i % 2) * 11, app.bottom() - 44 + (i / 2) * 11, 9, 9};
            fill(cr, s, 3, sw[i]);
            stroke(cr, s, 3, Color{1, 1, 1, 0.25f}, 0.8f);
        }
        text(cr, app.x + 44, app.bottom() - 47, "Apariencia", {ui(13, "Bold"), Color{1, 1, 1, 1}});
        std::string theme = config().theme == "adaptive" ? "Adaptativa · fondo" : config().theme;
        text(cr, app.x + 44, app.bottom() - 28, theme, {ui(11.5f), Color{1, 1, 1, 0.75f}});
        // Parent first, children after: later regions sit on top.
        ui_.add(id, app).click([this] { open_wallpaper_picker(); });
        w::icon_button(c, ID("cc.wall.next"), {app.right() - 42, app.bottom() - 44, 30, 30}, "󰒭", 14, false,
                       Color{1, 1, 1, 0.9f})
            .click([this] { wallpaper_next(); });
    }

    // ── column 2: player, weather, month ─────────────────────────────
    float c2 = 310, c2w = 262;
    Rect media{c2, y0, c2w, 128};
    w::card(c, media, 18);
    const auto& m = mpris_.info();
    if (!m.active) {
        glyph(cr, media.x + 36, media.cy(), "󰎊", 26, p.text_faint);
        text(cr, media.x + 62, media.cy() - 18, "Nada sonando", {ui(13.5f, "SemiBold"), p.text_dim});
        text(cr, media.x + 62, media.cy() + 1, "Reproduce algo para verlo aquí", {ui(11.5f), p.text_faint});
    } else {
        Color acc = media_accent();
        Rect art{media.x + 14, media.y + 14, 50, 50};
        draw_art(cr, art, 11);
        float tx = art.right() + 12, tww = media.right() - tx - 12;
        // Marquee for long titles.
        Text tt{ui(13.5f, "Bold"), p.text};
        Size ts = measure(m.title, tt);
        cairo_save(cr);
        cairo_rectangle(cr, tx, media.y + 14, tww, 22);
        cairo_clip(cr);
        float off = 0;
        if (ts.w > tww) {
            float over = ts.w - tww, dur = over / 26.0f, pause = 1.6f, cyc = 2 * pause + 2 * dur;
            float ph = std::fmod(anim_time_, cyc);
            off = ph < pause ? 0 : ph < pause + dur ? smoothstep((ph - pause) / dur) * over
                : ph < 2 * pause + dur ? over : over * (1 - smoothstep((ph - 2 * pause - dur) / dur));
        }
        text(cr, tx - off, media.y + 16, m.title, tt);
        cairo_restore(cr);
        text(cr, tx, media.y + 38, m.artist, {ui(11.5f), p.text_dim, Align::Left, tww});
        Rect pb{media.x + 14, media.y + 74, media.w - 28, 4};
        uint64_t sid = ID("cc.seek");
        bar(cr, pb.inset(0, -ui_.hover(sid)), mpris_.progress(), acc, p.text.with_alpha(0.12f));
        if (m.can_seek)
            ui_.add(sid, pb.inset(0, -8))
                .drag([this](const DragEvent& e) { mpris_.set_scrub(clamp01(e.fx)); })
                .release([this](const DragEvent& e) { mpris_.end_scrub(clamp01(e.fx)); });
        text(cr, pb.x, media.y + 86, fmt_duration(mpris_.position() / 1000000), {mono(10.5f), p.text_faint});
        if (m.length_us > 0)
            text(cr, pb.right(), media.y + 86, fmt_duration(m.length_us / 1000000), {mono(10.5f), p.text_faint, Align::Right});
        float mid = media.cx(), cy = media.y + 104;
        w::icon_button(c, ID("cc.prev"), {mid - 54, cy - 15, 30, 30}, "󰒮", 16).click([this] { mpris_.previous(); });
        w::icon_button(c, ID("cc.play"), {mid - 17, cy - 17, 34, 34}, m.playing ? "󰏤" : "󰐊", 19, true)
            .click([this] { mpris_.play_pause(); });
        w::icon_button(c, ID("cc.next"), {mid + 24, cy - 15, 30, 30}, "󰒭", 16).click([this] { mpris_.next(); });
    }

    Rect wcard{c2, y0 + 136, c2w, 124};
    {
        uint64_t id = ID("cc.weather");
        w::card(c, wcard, 18, ui_.hover(id));
        const auto& wd = weather_.data();
        if (wd.valid) {
            glyph(cr, wcard.x + 34, wcard.y + 36, wd.icon, 30, p.accent);
            text(cr, wcard.x + 62, wcard.y + 14, std::to_string(wd.temp) + "°", {ui(26, "Bold"), p.text});
            text(cr, wcard.x + 120, wcard.y + 18, wd.condition, {ui(12, "SemiBold"), p.text, Align::Left, wcard.w - 134});
            std::string sub = wd.location.empty() || demo_ ? "Sensación " + std::to_string(wd.feels) + "°"
                                                  : wd.location + " · " + std::to_string(wd.feels) + "°";
            text(cr, wcard.x + 120, wcard.y + 36, sub, {ui(11), p.text_dim, Align::Left, wcard.w - 134});
            size_t step = wd.hours.size() >= 9 ? 2 : 1;
            size_t n = std::min<size_t>(5, (wd.hours.size() + step - 1) / step);
            float hw = (wcard.w - 24) / std::max<size_t>(1, n);
            for (size_t i = 0; i < n; ++i) {
                const auto& hr = wd.hours[std::min(wd.hours.size() - 1, i * step)];
                float hx = wcard.x + 12 + hw * i + hw / 2;
                text(cr, hx, wcard.y + 68, hr.time, {mono(10), p.text_faint, Align::Center});
                glyph(cr, hx, wcard.y + 92, hr.icon, 14, p.text_dim);
                text(cr, hx, wcard.y + 102, std::to_string(hr.temp) + "°", {ui(11, "SemiBold"), p.text, Align::Center});
            }
        } else {
            glyph(cr, wcard.x + 34, wcard.cy(), "󰖐", 24, p.text_faint);
            text(cr, wcard.x + 62, wcard.cy() - 9, "Clima no disponible", {ui(12.5f), p.text_dim});
        }
        ui_.add(id, wcard).click([this] { open(View::Weather); });
    }

    Rect cal{c2, y0 + 268, c2w, h - (y0 + 268) - 16};
    {
        // The whole card opens the calendar (on the month shown); days and
        // arrows are registered after it, so they still win their clicks.
        uint64_t id = ID("cc.cal");
        float hv = ui_.hover(id);
        w::card(c, cal, 18, hv);
        ui_.add(id, cal).click([this] {
            cal_offset_ = cc_month_offset_;
            cal_day_ = -1;
            open(View::Calendar);
        });
        if (hv > 0.01f) {
            // An "open" hint slides in next to the arrows.
            float right = cal.right() - 14 - (cc_month_offset_ != 0 ? 112 : 60) - 6;
            Text t{ui(11.5f, "SemiBold"), p.accent.fade(hv)};
            Size ts = measure("Abrir", t);
            float x = right - ts.w - 18 + (1.0f - hv) * 8;
            text(cr, x, cal.y + 15, "Abrir", t);
            glyph(cr, x + ts.w + 9, cal.y + 23, "󰅂", 12, p.accent.fade(hv));
        }
    }
    draw_month(cr, cal.inset(14, 12), cc_month_offset_, -1, true, "cc.month",
               [this](int y, int m0, int d) {
                   struct tm now = local_tm();
                   cal_offset_ = (y - (now.tm_year + 1900)) * 12 + (m0 - now.tm_mon);
                   cal_day_ = d;
                   open(View::Calendar);
               },
               [this](int delta) {
                   cc_month_offset_ += delta;
                   request();
               });

    // ── column 3: notifications ──────────────────────────────────────
    float c3 = 584, c3w = w - c3 - 16;
    Rect nc{c3, y0, c3w, h - y0 - 16};
    w::card(c, nc, 18);
    static const std::vector<NotificationPtr> none;
    const auto& hist = demo_ ? none : notifs_.history();
    text(cr, nc.x + 16, nc.y + 14, "Notificaciones", {ui(14, "Bold"), p.text});
    if (!hist.empty())
        text(cr, nc.x + 16, nc.y + 33, std::to_string(hist.size()) + (notifs_.dnd() ? std::string(" · No molestar ") + Notifications::dnd_label(notifs_.dnd_mode()) : ""),
             {ui(11), notifs_.dnd() ? p.warn : p.text_faint});
    w::icon_button(c, ID("cc.n.dnd"), {nc.right() - 72, nc.y + 12, 28, 28},
                   notifs_.dnd_mode() == Notifications::DndTotal ? "󰂛" : notifs_.dnd() ? "󱏧" : "󰂚", 14, notifs_.dnd())
        .click([this] { notifs_.cycle_dnd(); });
    if (!hist.empty())
        w::icon_button(c, ID("cc.n.clear"), {nc.right() - 40, nc.y + 12, 28, 28}, "󰎟", 14).click([this] {
            notifs_.clear_history();
            cc_notif_scroll_.reset(0);
        });

    Rect list{nc.x + 10, nc.y + 54, nc.w - 20, nc.h - 64};
    if (hist.empty()) {
        glyph(cr, list.cx(), list.cy() - 14, "󰂜", 30, p.text_faint);
        text(cr, list.cx(), list.cy() + 10, "Nada nuevo", {ui(12.5f), p.text_faint, Align::Center});
    } else {
        // The scroll catcher goes underneath the cards so they keep their hover.
        float prev_content = cc_notif_content_;
        ui_.add(ID("cc.n.scroll"), list).scroll([this, list, prev_content](float dy) {
            float maxs = std::max(0.0f, prev_content - list.h);
            cc_notif_scroll_.set(std::clamp(cc_notif_scroll_.target + dy * 48, 0.0f, maxs));
        });
        cairo_save(cr);
        cairo_rectangle(cr, list.x - 2, list.y, list.w + 4, list.h);
        cairo_clip(cr);
        ui_.push_clip(list);
        float yy = list.y - cc_notif_scroll_.value;
        for (size_t i = 0; i < hist.size(); ++i) {
            const auto& n = hist[i];
            Text bt{ui(11.5f), p.text_dim, Align::Left, list.w - 56, 2, true, true};
            Size bs2 = n->body.empty() ? Size{} : measure(n->body, bt);
            float ch = 40 + bs2.h + 8;
            Rect card{list.x, yy, list.w, ch};
            if (card.bottom() >= list.y && card.y <= list.bottom()) {
                uint64_t id = ID("cc.n", n->id);
                float hv = ui_.hover(id);
                fill(cr, card, 14, mix(p.surface_hi.fade(0.6f), p.surface_hi, hv));
                Rect ic{card.x + 10, card.y + 10, 24, 24};
                cairo_surface_t* img = n->pixels.get();
                if (!img && !n->image.empty()) img = ImageCache::get().request(n->image, 48);
                if (img) image(cr, img, ic, starts_with(n->image, "icon:") && !n->pixels ? 4 : 12);
                else glyph(cr, ic.cx(), ic.cy(), n->urgency >= 2 ? "󰀪" : "󰂚", 13, n->urgency >= 2 ? p.bad : p.accent);
                std::string meta = (n->app.empty() ? "" : n->app + " · ") + time_ago(n->time);
                text(cr, card.x + 42, card.y + 8, meta, {ui(10.5f), p.text_faint, Align::Left, card.w - 76});
                text(cr, card.x + 42, card.y + 22, n->summary, {ui(12.5f, "SemiBold"), p.text, Align::Left, card.w - 60});
                if (!n->body.empty()) text(cr, card.x + 42, card.y + 40, n->body, bt);
                ui_.add(id, card).click([this, n] {
                    if (n->live || n->on_action) {
                        activate_notification(n);
                    } else {
                        niri_.focus_app(n->desktop_entry.empty() ? n->app : n->desktop_entry);
                    }
                    close_all();
                });
                if (hv > 0.05f)
                    w::icon_button(c, ID("cc.n.x", n->id), {card.right() - 28, card.y + 6, 22, 22}, "󰅖", 11)
                        .click([this, id = n->id] { notifs_.remove(id); });
            }
            yy += ch + 8;
        }
        float content = yy + cc_notif_scroll_.value - list.y;
        cc_notif_content_ = content;
        ui_.pop_clip();
        cairo_restore(cr);
        w::scrollbar(c, {list.right() + 3, list.y + 4, 3, list.h - 8}, cc_notif_scroll_.value, content, list.h);
    }
}

void App::wallpaper_next(int dir) {
    const std::string& cmd = config().cmd_wallpaper_next;
    if (cmd.empty() || cmd == "internal" || cmd.find("skwd") != std::string::npos) backdrop_.step(dir);
    else proc::spawn_shell(cmd);
}

void App::open_wallpaper_picker() {
    close_all();
    if (config().cmd_wallpaper_picker == "internal" || config().cmd_wallpaper_picker.empty()) wallpicker_.show();
    else proc::spawn_shell(config().cmd_wallpaper_picker);
}

bool App::airplane() const {
    return !net_.state().wifi_enabled && (!bt_.state().available || !bt_.state().powered);
}

void App::set_airplane(bool on) {
    net_.set_wifi(!on);
    if (bt_.state().available) bt_.set_powered(!on);
}

// ── Gaming Mode Pro: hybrid CPU affinity, I/O priority, kernel/GPU & niri ──

namespace {

struct CpuTopology {
    cpu_set_t p_cores;
    cpu_set_t e_cores;
    cpu_set_t all_cores;
    bool hybrid = false;
    int total = 0;
};

// Detects Intel/AMD hybrid core layouts (P-Cores vs E-Cores) via cpufreq max_freq.
const CpuTopology& cpu_topology() {
    static CpuTopology topo = [] {
        CpuTopology t;
        CPU_ZERO(&t.p_cores);
        CPU_ZERO(&t.e_cores);
        CPU_ZERO(&t.all_cores);
        std::vector<std::pair<int, long>> freqs;
        long max_f = 0, min_f = LONG_MAX;
        for (int cpu = 0; cpu < 128; ++cpu) {
            std::string s;
            if (!read_file("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq", s)) continue;
            long f = std::atol(s.c_str());
            if (f <= 0) continue;
            freqs.push_back({cpu, f});
            CPU_SET(cpu, &t.all_cores);
            max_f = std::max(max_f, f);
            min_f = std::min(min_f, f);
        }
        t.total = static_cast<int>(freqs.size());
        if (t.total > 0 && min_f < max_f * 9 / 10) {
            t.hybrid = true;
            for (auto& [cpu, f] : freqs) {
                if (f >= max_f * 9 / 10) CPU_SET(cpu, &t.p_cores);
                else CPU_SET(cpu, &t.e_cores);
            }
        } else {
            t.p_cores = t.all_cores;
            t.e_cores = t.all_cores;
        }
        return t;
    }();
    return topo;
}

// Applies CPU affinity and Best-Effort I/O priority to all threads of a process
// (and optionally its child processes).
void tune_process_tree(int root_pid, const cpu_set_t& mask, int ioprio_level, bool include_children) {
    if (root_pid <= 1) return;
    std::vector<int> pids{root_pid};
    if (include_children) {
        for (size_t idx = 0; idx < pids.size() && pids.size() < 64; ++idx) {
            int p = pids[idx];
            std::string task_dir = "/proc/" + std::to_string(p) + "/task";
            DIR* d = opendir(task_dir.c_str());
            if (!d) continue;
            while (struct dirent* e = readdir(d)) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
                std::string ch;
                if (!read_file(task_dir + "/" + e->d_name + "/children", ch)) continue;
                for (auto& tok : split(ch, ' ')) {
                    int cpid = std::atoi(tok.c_str());
                    if (cpid > 1 && std::find(pids.begin(), pids.end(), cpid) == pids.end() && pids.size() < 64)
                        pids.push_back(cpid);
                }
            }
            closedir(d);
        }
    }
    for (int p : pids) {
        std::string task_dir = "/proc/" + std::to_string(p) + "/task";
        DIR* d = opendir(task_dir.c_str());
        if (!d) {
            sched_setaffinity(p, sizeof(cpu_set_t), &mask);
            continue;
        }
        while (struct dirent* e = readdir(d)) {
            int tid = std::atoi(e->d_name);
            if (tid <= 0) continue;
            sched_setaffinity(tid, sizeof(cpu_set_t), &mask);
#ifdef SYS_ioprio_set
            if (ioprio_level >= 0) {
                // IOPRIO_WHO_PROCESS = 1, IOPRIO_CLASS_BE = 2
                syscall(SYS_ioprio_set, 1, tid, (2 << 13) | (ioprio_level & 7));
            }
#endif
        }
        closedir(d);
    }
}

bool is_game_window(const Window& win) {
    std::string app = lower(win.app_id);
    std::string title = lower(win.title);
    if (app.empty() && title.empty()) return false;
    static const char* kIgnore[] = {
        "steam", "lutris", "heroic", "com.heroicgameslauncher.hgl", "net.lutris.lutris",
        "discord", "vesktop", "brave", "firefox", "chromium", "chrome", "ghostty",
        "com.mitchellh.ghostty", "kitty", "alacritty", "wezterm", "nautilus", "org.gnome.",
        "code", "codium", "agy", "antigravity", "spotify", "obs", "vlc", "mpv", "pavucontrol"
    };
    for (const char* ig : kIgnore) {
        if (app == ig || starts_with(app, ig)) return false;
    }
    if (starts_with(app, "steam_app_") || starts_with(app, "steam_proton") ||
        starts_with(app, "gamescope") || ends_with(app, ".exe") ||
        app.find("wine") != std::string::npos || app.find("proton") != std::string::npos ||
        app.find("minecraft") != std::string::npos || app.find("prismlauncher") != std::string::npos ||
        app.find("retroarch") != std::string::npos || app.find("dolphin-emu") != std::string::npos ||
        app.find("pcsx2") != std::string::npos || app.find("rpcs3") != std::string::npos ||
        app.find("ryujinx") != std::string::npos || app.find("yuzu") != std::string::npos ||
        app.find("citra") != std::string::npos || app.find("cemu") != std::string::npos ||
        app.find("ppsspp") != std::string::npos || app.find("duckstation") != std::string::npos ||
        app.find("flycast") != std::string::npos || app.find("mgba") != std::string::npos) {
        return true;
    }
    if (win.pid > 1) {
        std::string env;
        if (read_file("/proc/" + std::to_string(win.pid) + "/environ", env)) {
            if (env.find("SteamAppId=") != std::string::npos ||
                env.find("STEAM_COMPAT_DATA_PATH=") != std::string::npos ||
                env.find("LUTRIS_GAME_UUID=") != std::string::npos ||
                env.find("WINEPREFIX=") != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

void write_sysfs(const char* path, const char* val) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fwrite(val, 1, strlen(val), f);
    fclose(f);
}

// Detects which hardware and kernel subsystems are present on this machine so
// hardware-specific gaming tunables only activate on compatible hardware.
struct HardwareCaps {
    bool intel_hwp = false;
    std::string intel_gt_card;
    std::string intel_gt_rp1;
    std::string intel_gt_rpn;
    bool mglru = false;
    bool sched_autogroup = false;
    bool dynamic_preempt = false;
};

const HardwareCaps& hardware_caps() {
    static HardwareCaps caps = [] {
        HardwareCaps c;
        std::string status;
        if (read_file("/sys/devices/system/cpu/intel_pstate/status", status) && trim(status) == "active" &&
            file_exists("/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost")) {
            c.intel_hwp = true;
        }
        for (int i = 0; i < 4; ++i) {
            std::string card = "/sys/class/drm/card" + std::to_string(i);
            std::string vendor, rp1, rpn;
            if (read_file(card + "/device/vendor", vendor) && trim(vendor) == "0x8086" &&
                read_file(card + "/gt_RP1_freq_mhz", rp1) && read_file(card + "/gt_RPn_freq_mhz", rpn) &&
                file_exists(card + "/gt_min_freq_mhz")) {
                c.intel_gt_card = card;
                c.intel_gt_rp1 = trim(rp1) + "\n";
                c.intel_gt_rpn = trim(rpn) + "\n";
                break;
            }
        }
        std::string lru;
        if (read_file("/sys/kernel/mm/lru_gen/enabled", lru) && trim(lru) != "0" && trim(lru) != "0x0000" &&
            file_exists("/sys/kernel/mm/lru_gen/min_ttl_ms")) {
            c.mglru = true;
        }
        c.sched_autogroup = file_exists("/proc/sys/kernel/sched_autogroup_enabled");
        c.dynamic_preempt = file_exists("/sys/kernel/debug/sched/preempt");
        return c;
    }();
    return caps;
}

void apply_kernel_gaming_tunables(bool on) {
    // Try privileged helper via non-interactive pkexec if installed.
    if (file_exists("/usr/local/libexec/niri-island-gaming-helper")) {
        proc::run_quiet({"pkexec", "--disable-internal-agent", "/usr/local/libexec/niri-island-gaming-helper", on ? "on" : "off"},
                        nullptr, 3000);
    }
    // Direct fallback writes gated by detected hardware capabilities.
    const auto& caps = hardware_caps();
    if (caps.intel_hwp) write_sysfs("/sys/devices/system/cpu/intel_pstate/hwp_dynamic_boost", on ? "1\n" : "0\n");
    if (!caps.intel_gt_card.empty())
        write_sysfs((caps.intel_gt_card + "/gt_min_freq_mhz").c_str(),
                    on ? caps.intel_gt_rp1.c_str() : caps.intel_gt_rpn.c_str());
    write_sysfs("/proc/sys/vm/compaction_proactiveness", on ? "0\n" : "20\n");
    write_sysfs("/proc/sys/vm/watermark_boost_factor", on ? "0\n" : "15000\n");
    write_sysfs("/sys/kernel/mm/transparent_hugepage/enabled", on ? "madvise\n" : "always\n");
    if (caps.mglru) write_sysfs("/sys/kernel/mm/lru_gen/min_ttl_ms", on ? "1000\n" : "0\n");
    if (caps.sched_autogroup) write_sysfs("/proc/sys/kernel/sched_autogroup_enabled", on ? "0\n" : "1\n");
    if (caps.dynamic_preempt) write_sysfs("/sys/kernel/debug/sched/preempt", on ? "full\n" : "lazy\n");
}

}  // namespace

void App::set_gaming_mode(bool on) {
    const auto& topo = cpu_topology();
    std::string active_detail = topo.hybrid ? "P-Cores · 0 blur · Fondo en pausa · Máx rendimiento"
                                            : "0 blur · Fondo en pausa · Máx rendimiento";
    if (gaming_mode_ == on) {
        OsdInfo o;
        o.kind = OsdInfo::Message;
        o.value = -1;
        o.glyph = "󰊴";
        o.muted = !on;
        o.label = on ? "Modo Gaming Pro activo" : "Modo Gaming desactivado";
        o.detail = on ? active_detail : "Efectos, fondo y energía normales";
        show_osd(o, 2.2);
        return;
    }
    gaming_mode_ = on;

    // 1. Save or restore power profile & Do Not Disturb mode
    if (on) {
        gaming_prev_profile_ = power_.profile();
        gaming_prev_dnd_ = notifs_.dnd_mode();
        power_.set_profile("performance");
        notifs_.set_dnd_mode(Notifications::DndTotal);
    } else {
        power_.set_profile(gaming_prev_profile_.empty() ? "balanced" : gaming_prev_profile_);
        notifs_.set_dnd_mode(gaming_prev_dnd_);
    }

    // 2. Shell ultra-light mode: zero springs, stop cava & spectrum, inhibit screensaver
    Spring::speed = on ? 0.0f : config().motion;
    if (on) {
        cava_.set_running(false);
        spec_live_ = false;
        if (spec_sub_) spec_sub_->hide();
        stop_screensaver();
    }
    setup_idle();

    // 3. Suspend animated wallpapers (Vulkan Engine, WebWall, mpvpaper) & mute wallpaper audio
    update_wallpaper_pause();
    if (config().wallpaper_engine_audio) backdrop_.set_audio(!on);

    // 4. Switch niri compositor mode.kdl + remove blur rules in niri-island.kdl
    std::string mode_kdl = home_dir() + "/.config/niri/cfg/mode.kdl";
    std::string gaming_kdl = home_dir() + "/.config/niri/cfg/gaming-mode.kdl";
    if (file_exists(gaming_kdl)) {
        std::string body = on ? "// Modo Gaming Activo\ninclude \"gaming-mode.kdl\"\n"
                              : "// Modo Normal Activo\ninclude \"layout.kdl\"\ninclude \"appearance.kdl\"\ninclude \"animations.kdl\"\n";
        write_file_atomic(mode_kdl, body);
    }
    apply_layout();
    niri_.reload_config();

    // 5. Hybrid CPU affinity (shell on E-Cores, games on P-Cores), NVMe I/O priority & kernel tunables
    if (on) {
        if (topo.hybrid) tune_process_tree(getpid(), topo.e_cores, -1, false);
        check_gaming_windows();
    } else {
        if (topo.total > 0) tune_process_tree(getpid(), topo.all_cores, -1, false);
        for (int pid : gaming_boosted_pids_) {
            if (topo.total > 0) tune_process_tree(pid, topo.all_cores, 4, true);
            if (user_bus_.get()) {
                sd_bus_call_method_async(user_bus_.get(), nullptr, "com.feralinteractive.GameMode",
                                         "/com/feralinteractive/GameMode", "com.feralinteractive.GameMode",
                                         "UnregisterGame", nullptr, nullptr, "i", pid);
            }
        }
        gaming_boosted_pids_.clear();
    }
    loop_.async([on] { apply_kernel_gaming_tunables(on); });

    // 6. Native OSD banner (shown even when DND Total is active)
    OsdInfo o;
    o.kind = OsdInfo::Message;
    o.value = -1;
    o.glyph = "󰊴";
    o.muted = !on;
    o.label = on ? "Modo Gaming Pro activado" : "Modo Gaming desactivado";
    o.detail = on ? active_detail : "Efectos, fondo y energía restaurados";
    show_osd(o, 2.4);
    request();
}

void App::check_gaming_windows() {
    const auto& wins = niri_.windows();
    // Prune closed windows from our prompted set
    for (auto it = gaming_prompted_wins_.begin(); it != gaming_prompted_wins_.end();) {
        if (wins.find(*it) == wins.end()) it = gaming_prompted_wins_.erase(it);
        else ++it;
    }

    if (gaming_mode_) {
        const auto& topo = cpu_topology();
        for (const auto& [id, win] : wins) {
            if (win.pid <= 1 || gaming_boosted_pids_.count(win.pid)) continue;
            if (!is_game_window(win)) continue;
            gaming_boosted_pids_.insert(win.pid);
            int pid = win.pid;
            loop_.async([pid, &topo] {
                if (topo.total > 0) tune_process_tree(pid, topo.p_cores, 0, true);
            });
            if (user_bus_.get()) {
                sd_bus_call_method_async(user_bus_.get(), nullptr, "com.feralinteractive.GameMode",
                                         "/com/feralinteractive/GameMode", "com.feralinteractive.GameMode",
                                         "RegisterGame", nullptr, nullptr, "i", pid);
            }
        }
        return;
    }

    // Interactive prompt when gaming_detect is enabled by the user in Settings
    if (!config().gaming_detect) return;
    for (const auto& [id, win] : wins) {
        if (gaming_prompted_wins_.count(id)) continue;
        if (!is_game_window(win)) continue;
        gaming_prompted_wins_.insert(id);
        auto n = std::make_shared<Notification>();
        n->app = "Modo Gaming";
        n->summary = "Juego detectado";
        n->body = (win.title.empty() ? win.app_id : win.title) + " — ¿Activar Modo Gaming Pro?";
        n->image = "icon:" + (win.app_id.empty() ? std::string("applications-games") : win.app_id);
        n->urgency = 1;
        n->timeout_ms = 10000;
        n->transient = true;
        n->actions = {{"on", "Activar"}, {"ignore", "Ignorar"}};
        n->on_action = [this](const std::string& act) {
            if (act == "on" || act == "default") set_gaming_mode(true);
        };
        notifs_.post(n);
        break;
    }
}

}  // namespace island
