// Screenshots and screen recording from the island: the options panel, the
// panel shown while recording, and the flows behind them.

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

void App::draw_capture(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    if (cap_windows_) {
        w::header(c, ID("capw.back"), w, "Capturar ventana", "Elige cuál").click([this] { cap_windows_ = false; });
        float y = 62;
        int i = 0;
        for (auto& [id, win] : niri_.windows()) {
            if (y + 50 > h - 10) break;
            Rect r{16, y, w - 32, 46};
            uint64_t wid = win.id;
            uint64_t rid = ID("capw.row", static_cast<int64_t>(wid));
            float hv = ui_.hover(rid);
            fill(cr, r, 14, mix(p.surface, p.surface_hi, hv));
            Rect ic{r.x + 10, r.cy() - 14, 28, 28};
            cairo_surface_t* img = ImageCache::get().request("icon:" + win.app_id, 56);
            if (img) image(cr, img, ic, 6);
            else glyph(cr, ic.cx(), ic.cy(), "󰖯", 15, p.text_dim);
            text(cr, r.x + 48, r.y + 6, win.title.empty() ? win.app_id : win.title, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 60});
            text(cr, r.x + 48, r.y + 25, win.app_id + (win.focused ? " · enfocada" : ""), {ui(11), p.text_dim, Align::Left, r.w - 60});
            ui_.add(rid, r).click([this, wid] {
                cap_windows_ = false;
                take_screenshot(2, cap_delay_, wid);
            });
            y += 52;
            ++i;
        }
        if (i == 0) text(cr, w / 2, h / 2, "No hay ventanas abiertas", {ui(13), p.text_dim, Align::Center});
        return;
    }

    w::header(c, ID("cap.back"), w, "Captura y grabación", "").click([this] { back(); });
    std::string pics = config().screenshot_dir.empty() ? user_dir("PICTURES", "Imágenes") + "/Capturas" : expand_home(config().screenshot_dir);
    std::string vids = config().recording_dir.empty() ? user_dir("VIDEOS", "Vídeos") + "/Grabaciones" : expand_home(config().recording_dir);
    w::icon_button(c, ID("cap.dirpics"), {w - 88, 14, 32, 32}, "󰉏", 15).click([pics] {
        make_dirs(pics);
        proc::spawn({"xdg-open", pics});
    });
    w::icon_button(c, ID("cap.dirvids"), {w - 50, 14, 32, 32}, "󰕧", 15).click([vids] {
        make_dirs(vids);
        proc::spawn({"xdg-open", vids});
    });

    // ── screenshot ───────────────────────────────────────────────────
    text(cr, 18, 62, "CAPTURA", {ui(11, "Bold"), p.accent});
    struct Big {
        const char* glyph;
        const char* label;
        int mode;
    };
    Big bigs[] = {{"󰩬", "Región", 0}, {"󰍹", "Pantalla", 1}, {"󰖯", "Ventana", 2}};
    float bw = (w - 32 - 20) / 3;
    for (int i = 0; i < 3; ++i) {
        Rect r{16 + i * (bw + 10), 82, bw, 76};
        uint64_t id = ID("cap.big", i);
        float hv = ui_.hover(id);
        float s = w::press_scale(ui_, id, 0.04f);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s);
        fill(cr, r, 18, mix(p.surface, p.surface_hi, hv));
        stroke(cr, r, 18, p.accent.with_alpha(0.15f + 0.3f * hv), 1);
        glyph(cr, r.cx(), r.y + 28, bigs[i].glyph, 22, mix(p.text_dim, p.accent, hv));
        text(cr, r.cx(), r.y + 48, bigs[i].label, {ui(12.5f, "SemiBold"), p.text, Align::Center});
        cairo_restore(cr);
        int mode = bigs[i].mode;
        ui_.add(id, r).click([this, mode] {
            if (mode == 2) {
                cap_windows_ = true;
                return;
            }
            take_screenshot(mode, cap_delay_, 0, cap_edit_);
        });
    }
    float y = 170;
    static const int delays[] = {0, 3, 5, 10};
    int di = 0;
    for (int i = 0; i < 4; ++i)
        if (delays[i] == cap_delay_) di = i;
    y += chips(c, "cap.delay", 16, y, w - 32, "Retardo", {"No", "3 s", "5 s", "10 s"}, di,
               [this](int i) { cap_delay_ = delays[i]; });
    y += chips(c, "cap.edit", 16, y, w - 32, "Pantalla", {"Abrir editor", "Guardar directo"}, cap_edit_ ? 0 : 1,
               [this](int i) { cap_edit_ = i == 0; });

    // ── recording ────────────────────────────────────────────────────
    y += 6;
    text(cr, 18, y, "GRABACIÓN", {ui(11, "Bold"), p.bad});
    y += 20;
    RecOptions& o = recorder_.options();
    auto save = [this] { recorder_.save_options(); };
    float half = (w - 32 - 12) / 2;
    chips(c, "rec.target", 16, y, half, "Zona", {"Pantalla", "Región"}, o.target, [&o, save](int i) { o.target = i; save(); });
    chips(c, "rec.fps", 16 + half + 12, y, half, "Fotogramas", {"30", "60"}, o.fps == 30 ? 0 : 1,
          [&o, save](int i) { o.fps = i == 0 ? 30 : 60; save(); });
    y += 40;
    y += chips(c, "rec.audio", 16, y, w - 32, "Audio", {"Sin audio", "Sistema", "Micrófono", "Ambos"}, o.audio,
               [&o, save](int i) { o.audio = i; save(); });
    chips(c, "rec.q", 16, y, half, "Calidad", {"Alta", "Media", "Baja"}, o.quality, [&o, save](int i) { o.quality = i; save(); });
    chips(c, "rec.enc", 16 + half + 12, y, half, "Codificador", {"GPU", "CPU"}, o.encoder,
          [&o, save](int i) { o.encoder = i; save(); });
    y += 40;
    static const int cds[] = {0, 3, 5, 10};
    int ci = 0;
    for (int i = 0; i < 4; ++i)
        if (cds[i] == o.countdown) ci = i;
    chips(c, "rec.fmt", 16, y, half, "Formato", {"MP4", "MKV", "GIF"}, o.format, [&o, save](int i) { o.format = i; save(); });
    chips(c, "rec.cd", 16 + half + 12, y, half, "Cuenta atrás", {"0", "3", "5", "10"}, ci,
          [&o, save](int i) { o.countdown = cds[i]; save(); });
    y += 46;
    bool busy = recorder_.active();
    w::pill(c, ID("rec.go"), {16, y, w - 32, 42}, busy ? "Abrir controles de la grabación" : "Empezar a grabar", "󰑋",
            busy ? w::Style::Tonal : w::Style::Danger, 13.5f)
        .click([this, busy] {
            if (busy) open(View::Record, true);
            else start_recording();
        });
}

void App::draw_record(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    (void)h;
    auto st = recorder_.state();
    if (st == Recorder::State::Idle) {
        text(cr, w / 2, 40, "No hay ninguna grabación en curso", {ui(13.5f, "SemiBold"), p.text_dim, Align::Center});
        w::pill(c, ID("recp.new"), {w / 2 - 110, 80, 220, 40}, "Opciones de grabación", "󰑋", w::Style::Danger)
            .click([this] { open(View::Capture, true); });
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
