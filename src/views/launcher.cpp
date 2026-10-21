// The launcher. The first character says what the field is for:
//   text  applications (ranked by use)     =  calculator
//   @     open windows                     !  countdown ("!25m té")
//   >     the island's own panels/actions  '  clipboard history
//   ?     web search

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

namespace {
struct Cmd {
    const char* glyph;
    const char* title;
    const char* keywords;
};
}  // namespace

void App::refresh_launcher() {
    results_.clear();
    std::string q = query_.text;
    std::string body = q.size() > 1 ? trim(q.substr(1)) : "";
    char prefix = q.empty() ? 0 : q[0];

    auto add_calc = [&](const std::string& expr) {
        double v;
        if (!calc_eval(expr, v)) return false;
        std::string res = calc_format(v);
        LauncherItem it;
        it.glyph = "󰃬";
        it.title = "= " + res;
        it.subtitle = expr;
        it.hint = "Copiar";
        it.run = [this, res] {
            loop_.async([res] { proc::run_quiet({"wl-copy"}, &res); });
            show_toast("Copiado: " + res);
        };
        results_.push_back(std::move(it));
        return true;
    };

    // Island commands, used for '>' and mixed into plain searches.
    struct Action {
        const char* glyph;
        std::string title, keywords;
        std::function<void()> run;
        bool close;
    };
    std::vector<Action> actions = {
        {"󰖩", "Wi-Fi", "wifi red internet", [this] { open(View::Wifi, true); }, false},
        {"󰂯", "Bluetooth", "bluetooth bt auriculares", [this] { open(View::Bluetooth, true); }, false},
        {"󰕾", "Sonido", "audio volumen salida sonido", [this] { open(View::Sound, true); }, false},
        {"󰖐", "Clima", "clima tiempo weather", [this] { open(View::Weather, true); }, false},
        {"󰃭", "Calendario", "calendario fecha agenda", [this] { open(View::Calendar, true); }, false},
        {"󰄲", "Tablero de tareas", "tareas kanban board todo", [this] { open(View::Board, true); }, false},
        {"󰠮", "Notas", "notas apuntes notes", [this] { open(View::Notes, true); }, false},
        {"󰓅", "Sistema", "cpu ram memoria sistema stats", [this] { open(View::Stats, true); }, false},
        {"󱎫", "Temporizador", "timer temporizador pomodoro", [this] { open(View::Timer, true); }, false},
        {"󰐥", "Sesión", "apagar reiniciar suspender salir power", [this] { open(View::Session, true); }, false},
        {"󰩬", "Capturar región", "captura screenshot region recorte", [this] { take_screenshot(0, 0); }, true},
        {"󰍹", "Capturar pantalla", "captura screenshot pantalla", [this] { take_screenshot(1, 0); }, true},
        {"󰑋", recorder_.active() ? "Detener grabación" : "Grabar pantalla", "grabar record video pantalla",
         [this] { recorder_.active() ? recorder_.stop() : start_recording(); }, true},
        {"󰄀", "Captura y grabación", "captura grabacion opciones", [this] { open_capbar(0); }, false},
        {"󰕮", "Centro de control", "control centro ajustes", [this] { open(View::Control, true); }, false},
        {"󰌾", "Bloquear pantalla", "bloquear lock", [this] { run_session("lock"); }, true},
        {"󰂛", notifs_.dnd() ? "Desactivar No molestar" : "Activar No molestar", "dnd no molestar silencio",
         [this] { notifs_.set_dnd(!notifs_.dnd()); }, true},
        {"󰀝", airplane() ? "Desactivar modo avión" : "Activar modo avión", "avion airplane",
         [this] { set_airplane(!airplane()); }, true},
        {"󰖔", "Luz nocturna", "luz nocturna night light", [] { proc::spawn_shell(config().cmd_night_light); }, true},
        {"󰸉", "Siguiente fondo", "fondo wallpaper siguiente", [this] { wallpaper_next(); }, true},
        {"󰋩", "Elegir fondo", "fondo wallpaper selector picker", [this] { open_wallpaper_picker(); }, true},
        {"󰝟", "Silenciar audio", "mute silenciar", [this] { audio_.toggle_mute(); }, true},
        {"󰑓", "Recargar configuración", "reload config recargar", [this] { command("reload"); }, true},
    };
    auto action_item = [&](const Action& a) {
        LauncherItem it;
        it.glyph = a.glyph;
        it.title = a.title;
        it.subtitle = "niri-island";
        it.run = a.run;
        it.close = a.close;
        return it;
    };
    auto match = [](const std::string& hay, const std::string& needle) {
        return needle.empty() || lower(hay).find(lower(needle)) != std::string::npos;
    };

    switch (prefix) {
        case '=':
            if (!body.empty() && !add_calc(body)) {
                LauncherItem it;
                it.glyph = "󰃬";
                it.title = "Expresión no válida";
                it.subtitle = "Ej.: 2^10, sqrt(2)*3, 15% de 80";
                results_.push_back(it);
            }
            break;
        case '@':
            for (auto& [id, win] : niri_.windows()) {
                if (!match(win.title + " " + win.app_id, body)) continue;
                LauncherItem it;
                it.icon = "icon:" + win.app_id;
                it.glyph = "󰖯";
                it.title = win.title.empty() ? win.app_id : win.title;
                it.subtitle = win.app_id + (win.focused ? " · enfocada" : "");
                uint64_t wid = win.id;
                it.run = [this, wid] { niri_.focus_window(wid); };
                results_.push_back(std::move(it));
                if (results_.size() >= 30) break;
            }
            break;
        case '!': {
            std::string label;
            double secs = Timer::parse(body, &label);
            if (secs > 0) {
                LauncherItem it;
                it.glyph = "󱎫";
                it.title = "Iniciar temporizador de " + fmt_duration(static_cast<int64_t>(secs));
                it.subtitle = label.empty() ? "Enter para empezar" : label;
                it.run = [this, secs, label] { timer_.start(secs, label); };
                results_.push_back(std::move(it));
            } else {
                LauncherItem it;
                it.glyph = "󱎫";
                it.title = timer_.running() ? "Temporizador en marcha: " + fmt_duration((int64_t)timer_.remaining())
                                            : "Escribe una duración";
                it.subtitle = "25m, 1h30, 90s, 5:00 — y una etiqueta opcional";
                it.run = [this] { open(View::Timer, true); };
                it.close = false;
                results_.push_back(std::move(it));
            }
            break;
        }
        case '>':
            for (auto& a : actions)
                if (match(a.title + " " + a.keywords, body)) results_.push_back(action_item(a));
            break;
        case '\'': {
            if (!proc::has("cliphist")) {
                LauncherItem it;
                it.glyph = "󰅌";
                it.title = "cliphist no está instalado";
                results_.push_back(it);
                break;
            }
            if (clip_items_.empty() && !clip_loading_) {
                clip_loading_ = true;
                auto out = std::make_shared<std::string>();
                loop_.async([out] { *out = proc::run({"cliphist", "list"}, 3000).out; },
                            [this, out] {
                                clip_loading_ = false;
                                clip_items_.clear();
                                for (auto& line : split(*out, '\n'))
                                    if (!line.empty() && clip_items_.size() < 200) clip_items_.push_back(line);
                                if (shown_ == View::Launcher) refresh_launcher();
                                request();
                            });
            }
            for (auto& line : clip_items_) {
                size_t tab = line.find('\t');
                std::string content = tab == std::string::npos ? line : line.substr(tab + 1);
                if (!match(content, body)) continue;
                LauncherItem it;
                bool img = content.find("[[ binary data") != std::string::npos;
                it.glyph = img ? "󰋩" : "󰅌";
                it.title = trim(replace_all(content, "\n", " "));
                it.subtitle = img ? "Imagen" : "Texto";
                it.run = [this, line] {
                    loop_.async([line] {
                        auto r = proc::run({"cliphist", "decode"}, 3000, &line);
                        if (r.ok()) proc::run_quiet({"wl-copy"}, &r.out);
                    });
                    show_toast("Copiado al portapapeles");
                };
                results_.push_back(std::move(it));
                if (results_.size() >= 40) break;
            }
            if (results_.empty()) {
                LauncherItem it;
                it.glyph = "󰅌";
                it.title = clip_loading_ ? "Cargando historial…" : "Historial vacío";
                results_.push_back(it);
            }
            break;
        }
        case '?': {
            LauncherItem it;
            it.glyph = "󰖟";
            it.title = body.empty() ? "Buscar en la web" : "Buscar «" + body + "»";
            it.subtitle = "DuckDuckGo";
            std::string term = body;
            it.run = [term] {
                std::string enc;
                for (unsigned char ch : term) {
                    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') enc += static_cast<char>(ch);
                    else if (ch == ' ') enc += '+';
                    else {
                        char b[4];
                        snprintf(b, sizeof(b), "%%%02X", ch);
                        enc += b;
                    }
                }
                proc::spawn({"xdg-open", "https://duckduckgo.com/?q=" + enc});
            };
            results_.push_back(std::move(it));
            break;
        }
        default: {
            // Maths typed straight in ("12*7") answers first.
            bool mathy = q.find_first_of("+-*/^%") != std::string::npos && q.find_first_of("0123456789") != std::string::npos;
            if (mathy) add_calc(q);
            for (auto& m : apps_.search(q, 24)) {
                const AppEntry* a = m.app;
                LauncherItem it;
                it.icon = a->icon.empty() ? "" : "icon:" + a->icon;
                it.glyph = "󰀻";
                it.title = a->name;
                it.subtitle = !a->generic.empty() ? a->generic : a->comment;
                std::string id = a->id;
                it.run = [this, id] {
                    for (auto& e : apps_.all())
                        if (e.id == id) {
                            apps_.launch(e);
                            break;
                        }
                };
                results_.push_back(std::move(it));
            }
            if (!q.empty())
                for (auto& a : actions)
                    if (match(a.title + " " + a.keywords, q) && results_.size() < 40) results_.push_back(action_item(a));
            if (!apps_.ready() && results_.empty()) {
                LauncherItem it;
                it.glyph = "󰔟";
                it.title = "Indexando aplicaciones…";
                results_.push_back(it);
            }
            break;
        }
    }
    if (sel_ >= static_cast<int>(results_.size())) sel_ = 0;
    request();
}

void App::launcher_activate(int index) {
    if (index < 0 || index >= static_cast<int>(results_.size())) return;
    LauncherItem it = results_[index];
    if (!it.run) return;
    if (it.close) close_all();
    it.run();
}

void App::draw_launcher(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    Rect field{14, 14, w - 28, 44};
    fill(cr, field, 22, p.surface_hi);
    std::string mode;
    char prefix = query_.text.empty() ? 0 : query_.text[0];
    switch (prefix) {
        case '=': mode = "Calcular"; break;
        case '@': mode = "Ventanas"; break;
        case '!': mode = "Temporizador"; break;
        case '>': mode = "Comandos"; break;
        case '\'': mode = "Portapapeles"; break;
        case '?': mode = "Web"; break;
        default: break;
    }
    glyph(cr, field.x + 22, field.cy(), "󰍉", 16, p.text_dim);
    float tx = field.x + 42;
    if (!mode.empty()) {
        Text mt{ui(11.5f, "Bold"), p.on_accent};
        float mw = measure(mode, mt).w + 18;
        fill(cr, {tx, field.cy() - 12, mw, 24}, 12, p.accent);
        text(cr, tx + 9, field.cy() - 8, mode, mt);
        tx += mw + 8;
    }
    Text qt{ui(15), p.text};
    if (query_.text.empty()) {
        text(cr, tx, field.cy() - 10, "Busca apps…   =  @  !  >  '  ?", {ui(15), p.text_faint});
    } else {
        text(cr, tx, field.cy() - 10, query_.text, {ui(15), p.text, Align::Left, field.right() - tx - 14});
    }
    if (std::fmod(anim_time_, 1.1f) < 0.7f) {
        float cx = caret_x(query_.text, qt, query_.caret);
        fill(cr, {tx + cx, field.cy() - 10, 2, 20}, 1, p.accent);
    }

    float y0 = 70, rowh = 46, gap = 4;
    Rect area{10, y0, w - 20, h - y0 - 10};
    if (results_.empty()) {
        text(cr, w / 2, y0 + 8, "Sin resultados", {ui(13), p.text_dim, Align::Center});
        return;
    }
    float content = results_.size() * (rowh + gap);
    ui_.add(ID("ln.area"), area).scroll([this, content, area](float dy) {
        launcher_scroll_.set(std::clamp(launcher_scroll_.target + dy * 50, 0.0f, std::max(0.0f, content - area.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, area.x - 4, area.y, area.w + 8, area.h);
    cairo_clip(cr);
    ui_.push_clip(area);
    for (size_t i = 0; i < results_.size(); ++i) {
        Rect r{area.x, area.y + i * (rowh + gap) - launcher_scroll_.value, area.w, rowh};
        if (r.bottom() < area.y || r.y > area.bottom()) continue;
        const auto& it = results_[i];
        uint64_t id = ID("ln.row", static_cast<int64_t>(i));
        bool sel = static_cast<int>(i) == sel_;
        float hv = ui_.hover(id);
        if (sel) fill(cr, r, 14, p.accent_dim);
        else if (hv > 0.01f) fill(cr, r, 14, p.text.with_alpha(0.05f * hv));
        Rect ic{r.x + 10, r.cy() - 16, 32, 32};
        cairo_surface_t* img = it.icon.empty() ? nullptr : ImageCache::get().request(it.icon, 64);
        if (img) image(cr, img, ic, 6);
        else glyph(cr, ic.cx(), ic.cy(), it.glyph, 17, sel ? p.accent : p.text_dim);
        float tw = r.w - 70 - (it.hint.empty() ? 0 : 70);
        if (it.subtitle.empty()) {
            text_in(cr, {r.x + 54, r.y, tw, r.h}, it.title, {ui(14, "SemiBold"), p.text});
        } else {
            text(cr, r.x + 54, r.y + 6, it.title, {ui(14, "SemiBold"), p.text, Align::Left, tw});
            text(cr, r.x + 54, r.y + 25, it.subtitle, {ui(11.5f), p.text_dim, Align::Left, tw});
        }
        if (sel) text(cr, r.right() - 14, r.cy() - 8, it.hint.empty() ? "↵" : it.hint, {ui(12, "SemiBold"), p.accent, Align::Right});
        int idx = static_cast<int>(i);
        ui_.add(id, r).click([this, idx] { launcher_activate(idx); });
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {w - 7, area.y + 4, 3, area.h - 8}, launcher_scroll_.value, content, area.h);
}

}  // namespace island
