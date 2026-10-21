// Screenshots and screen recording from the island: the options panel, the
// panel shown while recording, and the flows behind them.

#include <cmath>
#include <nlohmann/json.hpp>
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

static std::string focused_output_blocking() {
    auto r = proc::run({"niri", "msg", "--json", "focused-output"}, 3000);
    json j = json::parse(r.out, nullptr, false);
    if (j.is_object() && j.contains("name") && j["name"].is_string()) return j["name"];
    return "";
}

void App::with_output(std::function<void(const std::string&)> fn) {
    auto out = std::make_shared<std::string>();
    loop_.async([out] { *out = focused_output_blocking(); }, [out, fn] { fn(*out); });
}

void App::notify_file(const std::string& title, const std::string& path, cairo_surface_t* thumb, const std::string& body) {
    auto n = std::make_shared<Notification>();
    n->app = "niri-island";
    n->summary = title;
    std::string name = path.substr(path.rfind('/') + 1);
    n->body = escape_markup(body.empty() ? name : name + "  ·  " + body);
    if (thumb) n->pixels.reset(thumb, cairo_surface_destroy);
    n->has_default = true;
    n->actions = {{"open", "Abrir"}, {"folder", "Carpeta"}};
    n->timeout_ms = 7000;
    std::string dir = path.substr(0, path.rfind('/'));
    n->on_action = [path, dir](const std::string& a) {
        if (a == "folder") proc::spawn({"xdg-open", dir});
        else proc::spawn({"xdg-open", path});
    };
    notifs_.post(n);
}

void App::take_screenshot(int mode, int delay, uint64_t window_id, bool edit) {
    // mode: 0 region, 1 whole screen, 2 one window
    close_all();
    notifs_.dismiss_current();
    osd_.until = 0;
    auto shoot = [this, mode, window_id, edit] {
        island_hidden_ = true;
        request();
        // Two frames for the island to disappear from the screen.
        loop_.after(0.15, [this, mode, window_id, edit] {
            auto img = std::make_shared<cairo_surface_t*>(nullptr);
            loop_.async(
                [mode, window_id, img] {
                    if (mode == 2) {
                        // niri renders the window by itself and puts it in the clipboard.
                        proc::run({"niri", "msg", "action", "screenshot-window", "--id", std::to_string(window_id), "-d",
                                   "false", "-p", "false"},
                                  5000);
                        usleep(350000);
                        auto r = proc::run({"wl-paste", "--type", "image/png"}, 5000);
                        if (!r.ok() || r.out.empty()) return;
                        std::string file = runtime_dir() + "/niri-island-shot.png";
                        write_file_atomic(file, r.out);
                        *img = load_image_full(file);
                        unlink(file.c_str());
                        return;
                    }
                    std::string out = focused_output_blocking();
                    std::string file = runtime_dir() + "/niri-island-shot.ppm";
                    std::vector<std::string> cmd = {"grim", "-t", "ppm"};
                    if (!out.empty()) cmd.insert(cmd.end(), {"-o", out});
                    cmd.push_back(file);
                    proc::run(cmd, 8000);
                    *img = load_image_full(file);
                    unlink(file.c_str());
                },
                [this, img, mode, edit] {
                    island_hidden_ = false;
                    request();
                    if (!*img) {
                        show_toast("No se pudo capturar la pantalla");
                        return;
                    }
                    if (mode == 1 && !edit) {
                        Capture::save_async(*img, true, config().screenshot_copy,
                                            [this](const std::string& path, cairo_surface_t* thumb) {
                                                if (path.empty()) {
                                                    cairo_surface_destroy(thumb);
                                                    show_toast("No se pudo guardar la captura");
                                                    return;
                                                }
                                                notify_file("Captura guardada", path, thumb,
                                                            config().screenshot_copy ? "Copiada al portapapeles" : "");
                                            });
                        return;
                    }
                    capture_.edit(*img, mode != 0, mode != 2);
                });
        });
    };
    if (delay <= 0) {
        shoot();
        return;
    }
    // Countdown on the island, then shoot.
    for (int i = delay; i >= 1; --i) {
        loop_.after(delay - i, [this, i] {
            OsdInfo o;
            o.kind = OsdInfo::Message;
            o.value = -1;
            o.glyph = "󰄀";
            o.label = "Captura en " + std::to_string(i) + "…";
            show_osd(o, 0.95);
        });
    }
    loop_.after(delay, shoot);
}

void App::start_recording() {
    if (recorder_.active()) return;
    if (!proc::has("wf-recorder")) {
        show_toast("Instala wf-recorder para grabar");
        return;
    }
    close_all();
    recorder_.save_options();
    std::string sink = audio_.state().sink.name, source = audio_.state().source.name;
    auto go = [this, sink, source](const Rect& region) {
        with_output([this, region, sink, source](const std::string& out) { recorder_.start(region, out, sink, source); });
    };
    if (recorder_.options().target == 1) capture_.select_region(go, [] {});
    else go(Rect{});
}

// ── panels ─────────────────────────────────────────────────────────────────

// A row of mutually exclusive chips; returns the row's height.
static float chips(w::Ctx& c, const char* id, float x, float y, float w, const char* label,
                   const std::vector<std::string>& opts, int current, std::function<void(int)> pick) {
    const auto& p = T();
    text(c.cr, x, y + 8, label, {ui(12, "SemiBold"), p.text_dim});
    float cx = x + 112;
    float cw = (w - 112 - (opts.size() - 1) * 6.0f) / opts.size();
    for (size_t i = 0; i < opts.size(); ++i) {
        int idx = static_cast<int>(i);
        w::pill(c, ID(id, idx), {cx + i * (cw + 6), y, cw, 32}, opts[i], nullptr,
                idx == current ? w::Style::Accent : w::Style::Plain, 12)
            .click([pick, idx] { pick(idx); });
    }
    return 40;
}

// ── the capture bubble ────────────────────────────────────────────────────
// Rises from the bottom edge, docked like the island is at the top. Two
// clearly separate modes: Captura (region, screen, window) and Grabar (zone,
// audio, quality…), switched at the top.

void App::open_capbar(int mode) {
    close_all();
    capbar_mode_ = mode;
    cap_windows_ = false;
    capbar_leaving_ = false;
    if (!capbar_.exists()) {
        capbar_t_.reset(0);
        capbar_h_.reset(0);
    }
    capbar_t_.set(1);
    capbar_.open();
    capbar_.request();
}

void App::close_capbar(bool now) {
    if (!capbar_.exists()) return;
    if (now) {
        // Before a screenshot: gone at once, blur and all.
        capbar_.blur({});
        capbar_.destroy_now();
        wl_.dispatch_pending();
        return;
    }
    capbar_leaving_ = true;
    capbar_t_.set(0);
    capbar_.request();
}

void App::capbar_key(xkb_keysym_t sym, uint32_t mods) {
    (void)mods;
    if (sym == XKB_KEY_Escape) {
        if (cap_windows_) cap_windows_ = false;
        else close_capbar();
    } else if (sym == XKB_KEY_Tab || sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
        capbar_mode_ = 1 - capbar_mode_;
        cap_windows_ = false;
    } else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        // the obvious action of each mode
        close_capbar(true);
        if (capbar_mode_ == 0) take_screenshot(0, cap_delay_, 0, cap_edit_);
        else start_recording();
    }
    capbar_.request();
}

// The docked shape: square to the bottom edge, with the concave ears.
static void bottom_dock_path(cairo_t* cr, const Rect& r, float R, float e) {
    float H = r.bottom();
    cairo_new_path(cr);
    cairo_move_to(cr, r.x - e, H);
    cairo_arc_negative(cr, r.x - e, H - e, e, M_PI / 2, 0);
    cairo_line_to(cr, r.x, r.y + R);
    cairo_arc(cr, r.x + R, r.y + R, R, M_PI, 3 * M_PI / 2);
    cairo_line_to(cr, r.right() - R, r.y);
    cairo_arc(cr, r.right() - R, r.y + R, R, -M_PI / 2, 0);
    cairo_line_to(cr, r.right(), H - e);
    cairo_arc_negative(cr, r.right() + e, H - e, e, M_PI, M_PI / 2);
    cairo_close_path(cr);
}

// A big choice tile (glyph and label); `on` keeps it lit.
static Region& tile(w::Ctx& c, uint64_t id, const Rect& r, const char* g, const char* label, const char* sub, bool on,
                    Color tint) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    float s = w::press_scale(c.ui, id, 0.04f);
    cairo_save(c.cr);
    scale_around(c.cr, r.cx(), r.cy(), s);
    fill(c.cr, r, 18, on ? tint.with_alpha(0.22f) : mix(p.surface, p.surface_hi, hv));
    stroke(c.cr, r, 18, tint.with_alpha(on ? 0.7f : 0.12f + 0.3f * hv), on ? 1.5f : 1);
    glyph(c.cr, r.cx(), r.y + 26, g, 22, on ? tint : mix(p.text_dim, tint, hv));
    text(c.cr, r.cx(), r.y + 44, label, {ui(12.5f, "SemiBold"), p.text, Align::Center});
    if (sub) text(c.cr, r.x + 6, r.y + 61, sub, {ui(10.5f), p.text_faint, Align::Center, r.w - 12});
    cairo_restore(c.cr);
    return c.ui.add(id, r);
}

void App::draw_capbar(cairo_t* cr, Ui& u, float W, float H, float dt) {
    const auto& p = T();
    w::Ctx c{cr, u};
    const float BW = 640, R = 26, E = 14;
    bool rec_busy = recorder_.active();
    int nwin = 0;
    for (auto& kv : niri_.windows()) {
        (void)kv;
        ++nwin;
    }
    // the height each mode needs
    float want = cap_windows_ ? 64 + std::min(nwin, 6) * 52.0f + 14
               : capbar_mode_ == 0 ? 64 + 84 + 12 + 80 + 16
               : rec_busy ? 64 + 120
                          : 64 + 84 + 12 + 120 + 8 + 44 + 18;
    if (capbar_h_.value <= 0.5f) capbar_h_.reset(want);
    capbar_h_.set(want);
    capbar_h_.update(dt);
    capbar_t_.update(dt);
    if (capbar_leaving_ && capbar_t_.value < 0.02f) {
        capbar_leaving_ = false;
        capbar_.blur({});
        capbar_.destroy_now();
        return;
    }
    float t = std::clamp(capbar_t_.value, 0.0f, 1.1f);
    float bh = capbar_h_.value;
    Rect shape{std::round(W / 2 - BW / 2), H - bh * std::min(t, 1.0f), BW, bh * std::min(t, 1.0f)};
    capbar_rect_ = shape;
    // the body (and niri's blur behind it)
    float op = config().glass ? config().island_opacity : 1.0f;
    bottom_dock_path(cr, shape, std::min(R, shape.h / 2), E);
    set(cr, p.island.with_alpha(op));
    cairo_fill(cr);
    if (config().glass && config().compositor_blur && wl_.can_blur() && t > 0.05f)
        capbar_.blur({{Rect{shape.x, shape.y, shape.w, shape.h + R}, R, false}});  // square at the edge
    else capbar_.blur({});
    float ca = std::clamp((t - 0.35f) / 0.65f, 0.0f, 1.0f);
    if (ca <= 0.01f) return;

    // content: laid out for the full height, sliding up with the body
    float y0 = H - bh + (1 - std::min(t, 1.0f)) * bh;
    float x0 = shape.x;
    cairo_save(cr);
    bottom_dock_path(cr, shape, std::min(R, shape.h / 2), E);
    cairo_clip(cr);
    cairo_push_group(cr);
    u.push_clip(shape);

    // ── top row: the two modes, clearly apart ──
    Rect seg{std::round(W / 2 - 150), y0 + 14, 300, 36};
    fill(cr, seg, 18, p.surface);
    const char* names[2] = {"Captura", "Grabar"};
    const char* glyphs[2] = {"󰄀", "󰑋"};
    for (int m = 0; m < 2; ++m) {
        Rect half{seg.x + 3 + m * (seg.w / 2 - 3), seg.y + 3, seg.w / 2 - 3, seg.h - 6};
        bool on = capbar_mode_ == m;
        Color tint = m == 0 ? p.accent : p.bad;
        uint64_t id = ID("cb.mode", m);
        float hv = u.hover(id);
        if (on) fill(cr, half, half.h / 2, tint);
        else if (hv > 0.01f) fill(cr, half, half.h / 2, p.text.with_alpha(0.06f * hv));
        Color fg = on ? (m == 0 ? p.on_accent : Color{1, 1, 1, 1}) : p.text_dim;
        float tw = measure(names[m], {ui(13, "SemiBold")}).w;
        float gx = half.cx() - (tw + 22) / 2;
        glyph(cr, gx + 8, half.cy(), glyphs[m], 14, fg);
        text(cr, gx + 22, half.cy() - 9, names[m], {ui(13, "SemiBold"), fg});
        u.add(id, half).click([this, m] {
            capbar_mode_ = m;
            cap_windows_ = false;
        });
    }
    std::string pics = config().screenshot_dir.empty() ? user_dir("PICTURES", "Imágenes") + "/Capturas" : expand_home(config().screenshot_dir);
    std::string vids = config().recording_dir.empty() ? user_dir("VIDEOS", "Vídeos") + "/Grabaciones" : expand_home(config().recording_dir);
    std::string dir = capbar_mode_ == 0 ? pics : vids;
    w::icon_button(c, ID("cb.dir"), {x0 + 16, y0 + 16, 32, 32}, "󰉏", 15).click([this, dir] {
        make_dirs(dir);
        proc::spawn({"xdg-open", dir});
        close_capbar();
    });
    w::icon_button(c, ID("cb.close"), {x0 + BW - 48, y0 + 16, 32, 32}, "󰅖", 14).click([this] { close_capbar(); });

    float y = y0 + 64;
    if (cap_windows_) {
        // ── a window to capture ──
        int i = 0;
        for (auto& [id, win] : niri_.windows()) {
            if (i >= 6) break;
            Rect r{x0 + 16, y, BW - 32, 46};
            uint64_t wid = win.id;
            uint64_t rid = ID("cb.win", static_cast<int64_t>(wid));
            float hv = u.hover(rid);
            fill(cr, r, 14, mix(p.surface, p.surface_hi, hv));
            Rect ic{r.x + 10, r.cy() - 14, 28, 28};
            if (cairo_surface_t* img = ImageCache::get().request("icon:" + win.app_id, 56)) image(cr, img, ic, 6);
            else glyph(cr, ic.cx(), ic.cy(), "󰖯", 15, p.text_dim);
            text(cr, r.x + 48, r.y + 6, win.title.empty() ? win.app_id : win.title, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 60});
            text(cr, r.x + 48, r.y + 25, win.app_id + (win.focused ? " · enfocada" : ""), {ui(11), p.text_dim, Align::Left, r.w - 60});
            u.add(rid, r).click([this, wid] {
                cap_windows_ = false;
                close_capbar(true);
                take_screenshot(2, cap_delay_, wid);
            });
            y += 52;
            ++i;
        }
        if (i == 0) text(cr, W / 2, y + 4, "No hay ventanas abiertas", {ui(13), p.text_dim, Align::Center});
    } else if (capbar_mode_ == 0) {
        // ── screenshot ──
        struct Big {
            const char* glyph;
            const char* label;
            const char* sub;
        };
        Big bigs[3] = {{"󰩬", "Región", "Arrastra una zona"}, {"󰍹", "Pantalla", "Toda la pantalla"}, {"󰖯", "Ventana", "Elige una ventana"}};
        float bw = (BW - 32 - 20) / 3;
        for (int i = 0; i < 3; ++i) {
            Rect r{x0 + 16 + i * (bw + 10), y, bw, 76};
            tile(c, ID("cb.shot", i), r, bigs[i].glyph, bigs[i].label, bigs[i].sub, false, p.accent).click([this, i] {
                if (i == 2) {
                    cap_windows_ = true;
                    return;
                }
                close_capbar(true);
                take_screenshot(i, cap_delay_, 0, cap_edit_);
            });
        }
        y += 88;
        static const int delays[] = {0, 3, 5, 10};
        int di = 0;
        for (int i = 0; i < 4; ++i)
            if (delays[i] == cap_delay_) di = i;
        y += chips(c, "cb.delay", x0 + 16, y, BW - 32, "Retardo", {"No", "3 s", "5 s", "10 s"}, di,
                   [this](int i) { cap_delay_ = delays[i]; });
        chips(c, "cb.edit", x0 + 16, y, BW - 32, "Al capturar", {"Abrir el editor", "Guardar directamente"}, cap_edit_ ? 0 : 1,
              [this](int i) { cap_edit_ = i == 0; });
    } else if (rec_busy) {
        // ── a recording is running ──
        text(cr, W / 2, y + 6, "Hay una grabación en curso", {ui(14, "SemiBold"), p.text, Align::Center});
        float bw2 = (BW - 32 - 10) / 2;
        w::pill(c, ID("cb.recctl"), {x0 + 16, y + 44, bw2, 42}, "Controles", "󰐊", w::Style::Tonal, 13).click([this] {
            close_capbar();
            open(View::Record, true);
        });
        w::pill(c, ID("cb.recstop"), {x0 + 26 + bw2, y + 44, bw2, 42}, "Detener y guardar", "󰓛", w::Style::Danger, 13).click([this] {
            close_capbar();
            recorder_.stop();
        });
    } else {
        // ── recording ──
        RecOptions& o = recorder_.options();
        auto save = [this] { recorder_.save_options(); };
        float bw = (BW - 32 - 10) / 2;
        const char* tg[2] = {"󰍹", "󰩬"};
        const char* tl[2] = {"Pantalla", "Región"};
        const char* ts[2] = {"Toda la pantalla", "La eliges al empezar"};
        for (int i = 0; i < 2; ++i)
            tile(c, ID("cb.zone", i), {x0 + 16 + i * (bw + 10), y, bw, 76}, tg[i], tl[i], ts[i], o.target == i, p.bad)
                .click([&o, save, i] {
                    o.target = i;
                    save();
                });
        y += 88;
        float half = (BW - 32 - 12) / 2;
        y += chips(c, "cb.audio", x0 + 16, y, BW - 32, "Audio", {"Sin audio", "Sistema", "Micrófono", "Ambos"}, o.audio,
                   [&o, save](int i) { o.audio = i; save(); });
        chips(c, "cb.fps", x0 + 16, y, half, "Fotogramas", {"30", "60"}, o.fps == 30 ? 0 : 1,
              [&o, save](int i) { o.fps = i == 0 ? 30 : 60; save(); });
        chips(c, "cb.fmt", x0 + 28 + half, y, half, "Formato", {"MP4", "MKV", "GIF"}, o.format,
              [&o, save](int i) { o.format = i; save(); });
        y += 40;
        static const int cds[] = {0, 3, 5, 10};
        int ci = 0;
        for (int i = 0; i < 4; ++i)
            if (cds[i] == o.countdown) ci = i;
        chips(c, "cb.q", x0 + 16, y, half, "Calidad", {"Alta", "Media", "Baja"}, o.quality, [&o, save](int i) { o.quality = i; save(); });
        chips(c, "cb.cd", x0 + 28 + half, y, half, "Cuenta atrás", {"0", "3", "5", "10"}, ci,
              [&o, save](int i) { o.countdown = cds[i]; save(); });
        y += 48;
        w::pill(c, ID("cb.recgo"), {x0 + 16, y, BW - 32, 44}, o.target == 1 ? "Elegir región y grabar" : "Empezar a grabar", "󰑋",
                w::Style::Danger, 13.5f)
            .click([this] {
                close_capbar(true);
                start_recording();
            });
    }
    u.pop_clip();
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, ca);
    cairo_restore(cr);
}

void App::draw_record(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    (void)h;
    auto st = recorder_.state();
    if (st == Recorder::State::Idle) {
        text(cr, w / 2, 40, "No hay ninguna grabación en curso", {ui(13.5f, "SemiBold"), p.text_dim, Align::Center});
        w::pill(c, ID("recp.new"), {w / 2 - 110, 80, 220, 40}, "Opciones de grabación", "󰑋", w::Style::Danger)
            .click([this] { open_capbar(1); });
        return;
    }
    float pulse = st == Recorder::State::Recording ? 0.55f + 0.45f * std::fabs(std::sin(anim_time_ * 2.2f)) : 1.0f;
    Color tint = st == Recorder::State::Paused ? p.warn : p.bad;
    fill(cr, {22, 24, 16, 16}, 8, tint.fade(pulse));
    const char* state = st == Recorder::State::Countdown ? "Preparando…" : st == Recorder::State::Paused ? "En pausa"
                      : st == Recorder::State::Saving    ? "Guardando…"  : "Grabando";
    text(cr, 48, 20, state, {ui(15, "Bold"), p.text});
    std::string t = st == Recorder::State::Countdown ? std::to_string(recorder_.countdown())
                                                     : fmt_duration(static_cast<int64_t>(recorder_.elapsed()));
    text(cr, w - 20, 12, t, {mono(30, "Bold"), tint, Align::Right});

    const RecOptions& o = recorder_.options();
    static const char* audio[] = {"sin audio", "audio del sistema", "micrófono", "sistema + micrófono"};
    static const char* fmt[] = {"MP4", "MKV", "GIF"};
    char info[160];
    snprintf(info, sizeof(info), "%s  ·  %d fps  ·  %s  ·  %s  ·  %.1f MB", recorder_.has_region() ? "Región" : "Pantalla",
             o.fps, o.encoder == 0 ? "GPU" : "CPU", audio[o.audio], recorder_.bytes() / 1048576.0);
    text(cr, 48, 46, std::string(info) + "  ·  " + fmt[o.format], {ui(11.5f), p.text_dim, Align::Left, w - 70});

    float y = 96, bw = (w - 32 - 3 * 8) / 4;
    bool paused = st == Recorder::State::Paused;
    bool live = st == Recorder::State::Recording || paused;
    w::pill(c, ID("recp.pause"), {16, y, bw, 44}, paused ? "Seguir" : "Pausa", paused ? "󰐊" : "󰏤",
            live ? w::Style::Tonal : w::Style::Ghost)
        .click([this] { recorder_.toggle_pause(); });
    bool mic_muted = audio_.state().source.muted;
    w::pill(c, ID("recp.mic"), {16 + (bw + 8), y, bw, 44}, mic_muted ? "Mic off" : "Mic on", mic_muted ? "󰍭" : "󰍬",
            mic_muted ? w::Style::Plain : w::Style::Tonal)
        .click([this] { audio_.toggle_mic_mute(); });
    w::pill(c, ID("recp.stop"), {16 + 2 * (bw + 8), y, bw, 44}, "Detener", "󰓛", w::Style::Danger).click([this] {
        recorder_.stop();
        close_all();
    });
    w::pill(c, ID("recp.cancel"), {16 + 3 * (bw + 8), y, bw, 44}, "Descartar", "󰆴", w::Style::Ghost).click([this] {
        recorder_.cancel();
        close_all();
        show_toast("Grabación descartada");
    });
    text(cr, w / 2, 156, "Clic en el punto rojo de la isla para detener", {ui(11), p.text_faint, Align::Center});
}

}  // namespace island
