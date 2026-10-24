// The settings centre: a window over a dimmed desktop, a sidebar of sections
// with a search, and cards of toggles, choices, sliders and fields. Every
// change goes straight into config.json, which the island reloads live.

#include <algorithm>
#include <map>

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/workshop.hpp"
#include "suite/webwall.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

namespace {

struct SetRow {
    enum Kind { Toggle, Choice, Slider, Text, Info, Buttons, Preview, Custom } kind = Info;
    int section = 0;
    std::string group, title, sub;
    std::function<bool()> get_b;
    std::function<void(bool)> set_b;
    std::vector<std::string> options;
    std::function<int()> get_i;
    std::function<void(int)> set_i;
    float lo = 0, hi = 1;
    std::function<float()> get_f;
    std::function<void(float)> set_f;
    std::function<std::string(float)> fmt;
    std::string key;
    std::function<std::string()> get_s;
    std::function<void(const std::string&)> set_s;
    std::vector<std::pair<std::string, std::function<void()>>> buttons;
    std::function<void(cairo_t*, const Rect&, int, float)> preview;
    std::function<void(cairo_t*, const Rect&)> custom;
    float custom_h = 0;
    std::string keywords;  // found by the search too (options inside a custom row)
};

struct Section {
    const char* glyph;
    const char* title;
    const char* about;
};

const Section kSections[] = {
    {"󰏘", "Apariencia", "Colores, tipografía y movimiento"},
    {"󰍹", "Barra e isla", "Cómo se reparten los módulos y dónde vive la isla"},
    {"󰂵", "Cristal", "Transparencia y desenfoque del compositor"},
    {"󰂚", "Notificaciones", "No molestar, duración e historial"},
    {"󰄀", "Capturas y grabación", "Valores por defecto de la suite de captura"},
    {"󰸉", "Fondos de pantalla", "Carpetas y aplicación de fondos"},
    {"󰒓", "Sistema", "Comandos, terminal y tipografías"},
    {"󰋽", "Acerca de", "Versión y archivo de configuración"},
};
constexpr int kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

// Mini screens for the layout / style choices.
void preview_layout(cairo_t* cr, const Rect& r, int which, float sel) {
    const auto& p = T();
    fill(cr, r, 12, mix(p.surface, p.surface_hi, 0.5f));
    Rect scr = r.inset(10);
    cairo_pattern_t* g = cairo_pattern_create_linear(scr.x, scr.y, scr.right(), scr.bottom());
    cairo_pattern_add_color_stop_rgba(g, 0, p.accent.r, p.accent.g, p.accent.b, 0.35);
    cairo_pattern_add_color_stop_rgba(g, 1, p.accent_dim.r, p.accent_dim.g, p.accent_dim.b, 0.6);
    rrect(cr, scr, 8);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    Color cap = p.island;
    float y = scr.y + 7, hh = 10, cx = scr.cx();
    auto pill = [&](float x, float w) { fill(cr, {x, y, w, hh}, hh / 2, cap); };
    if (which == 0) {  // grouped
        pill(cx - 18, 36);
        pill(cx - 18 - 6 - 22, 22);
        pill(cx + 18 + 6, 26);
    } else if (which == 1) {  // spread
        pill(cx - 18, 36);
        pill(scr.x + 6, 30);
        pill(scr.right() - 6 - 30, 30);
    } else {  // one island
        pill(cx - 46, 92);
    }
    (void)sel;
}

void preview_style(cairo_t* cr, const Rect& r, int which, float) {
    const auto& p = T();
    fill(cr, r, 12, mix(p.surface, p.surface_hi, 0.5f));
    Rect scr = r.inset(10);
    rrect(cr, scr, 8);
    set(cr, p.accent.with_alpha(0.3f));
    cairo_fill(cr);
    Rect isl{scr.cx() - 34, which == 0 ? scr.y + 7 : scr.y, 68, 18};
    cairo_save(cr);
    rrect(cr, scr, 8);
    cairo_clip(cr);
    App::island_outline(cr, isl, 9, which == 1);
    set(cr, p.island);
    cairo_fill(cr);
    cairo_restore(cr);
}

// The wallpaper picker's styles, in miniature.
void preview_picker(cairo_t* cr, const Rect& r, int which, float) {
    const auto& p = T();
    fill(cr, r, 12, mix(p.surface, p.surface_hi, 0.5f));
    Rect scr = r.inset(10);
    rrect(cr, scr, 8);
    set(cr, p.island.with_alpha(0.9f));
    cairo_fill(cr);
    cairo_save(cr);
    rrect(cr, scr, 8);
    cairo_clip(cr);
    Color a = p.accent, b = p.accent.with_alpha(0.55f), c = p.text.with_alpha(0.35f);
    if (which == 0) {
        // the carousel: skewed slices, the middle one wide
        float sw[5] = {14, 14, 38, 14, 14};
        float x = scr.cx() - (14 * 4 + 38 + 4 * 4) / 2;
        for (int i = 0; i < 5; ++i) {
            float y0 = scr.y + 8, y1 = scr.bottom() - 8, k = 6;
            cairo_new_path(cr);
            cairo_move_to(cr, x + k, y0);
            cairo_line_to(cr, x + sw[i] + k, y0);
            cairo_line_to(cr, x + sw[i] - k, y1);
            cairo_line_to(cr, x - k, y1);
            cairo_close_path(cr);
            set(cr, i == 2 ? a : i % 2 ? b : c);
            cairo_fill(cr);
            x += sw[i] + 4;
        }
    } else if (which == 1) {
        // the honeycomb
        float R = 11, cs = 1.5f * R + 1.5f, rs = std::sqrt(3.0f) * R + 1.5f;
        for (int col = 0; col < 7; ++col)
            for (int row = 0; row < 3; ++row) {
                float cx = scr.x + 8 + col * cs, cy = scr.y + 6 + row * rs + (col % 2 ? rs / 2 : 0);
                cairo_new_path(cr);
                for (int k = 0; k < 6; ++k) {
                    double an = M_PI / 3 * k;
                    double px = cx + R * std::cos(an), py = cy + R * std::sin(an);
                    k ? cairo_line_to(cr, px, py) : cairo_move_to(cr, px, py);
                }
                cairo_close_path(cr);
                set(cr, (col + row) % 3 == 0 ? a : (col + row) % 3 == 1 ? b : c);
                cairo_fill(cr);
            }
    } else {
        // wallhaven's cards
        float gw = scr.w - 16, cw = (gw - 2 * 5) / 3, ch = cw * 9 / 16;
        for (int i = 0; i < 6; ++i) {
            Rect q{scr.x + 8 + (i % 3) * (cw + 5), scr.y + 8 + (i / 3) * (ch + 5), cw, ch};
            fill(cr, q, 3, i == 1 ? a : i % 2 ? b : c);
        }
    }
    cairo_restore(cr);
}

}  // namespace

// The bar's modules: on/off, side, and order.
// The list is shown as the bar is: the left group, then the right one, each
// left to right on screen. Changing a module's side moves it there (next to
// the island), and moving past the edge of a group changes its side.
static void sort_modules(std::vector<ModuleConf>& ms) {
    std::stable_partition(ms.begin(), ms.end(), [](const ModuleConf& m) { return m.side == "left"; });
}

static size_t left_count(const std::vector<ModuleConf>& ms) {
    return static_cast<size_t>(std::count_if(ms.begin(), ms.end(), [](const ModuleConf& m) { return m.side == "left"; }));
}

// What the options inside a module's card are called, for the search.
std::string App::module_option_words(const std::string& id) const {
    if (id == "perf")
        return "monitor rendimiento procesador cpu memoria ram temperatura red disco swap intercambio estilo números "
               "anillos cápsulas juntas separadas";
    if (id == "battery") return "batería porcentaje";
    return "";
}

float App::module_options_h(const std::string& id) const {
    if (id == "perf") return 140;
    if (id == "battery") return 44;
    return 0;
}

float App::module_extra_h(const std::string& id) const {
    auto it = mod_open_.find(id);
    if (it == mod_open_.end()) return 0;
    return std::round(module_options_h(id) * clamp01(it->second.value));
}

void App::draw_module_options(cairo_t* cr, Ui& u, const std::string& id, const Rect& area) {
    const auto& p = T();
    w::Ctx c{cr, u};
    auto& cfg = config();
    if (id == "battery") {
        text(cr, area.x, area.y + 6, "Mostrar el porcentaje", {ui(12.5f), p.text, Align::Left});
        text(cr, area.x, area.y + 23, "El número junto al icono", {ui(11), p.text_dim, Align::Left});
        w::toggle(c, ID("set.mod.batpct"), {area.right() - 54, area.y + 8, 46, 26}, cfg.battery_percent, [this] {
            config().battery_percent = !config().battery_percent;
            save_config_soon();
        });
        return;
    }
    if (id == "perf") {
        // what it shows: pills that light up
        static const std::pair<const char*, const char*> metrics[] = {
            {"cpu", "CPU"}, {"ram", "RAM"}, {"temp", "Temperatura"}, {"net", "Red"}, {"disk", "Disco"}, {"swap", "Swap"}};
        static const std::vector<std::string> order = {"cpu", "ram", "temp", "net", "disk", "swap"};
        text(cr, area.x, area.y + 12, "Mostrar", {ui(12), p.text_dim, Align::Left});
        float x = area.x + 86, pw = (area.right() - x - 8 - 5 * 6) / 6;
        for (int k = 0; k < 6; ++k) {
            std::string mid = metrics[k].first;
            bool on = std::find(cfg.perf_metrics.begin(), cfg.perf_metrics.end(), mid) != cfg.perf_metrics.end();
            w::pill(c, ID("set.perf.m", k), {x + k * (pw + 6), area.y + 4, pw, 30}, metrics[k].second, nullptr,
                    on ? w::Style::Accent : w::Style::Plain, 11.5f)
                .click([this, mid] {
                    auto& pm = config().perf_metrics;
                    if (std::find(pm.begin(), pm.end(), mid) != pm.end()) pm.erase(std::remove(pm.begin(), pm.end(), mid), pm.end());
                    else {
                        pm.push_back(mid);
                        std::sort(pm.begin(), pm.end(), [](const std::string& a, const std::string& b) {
                            return std::find(order.begin(), order.end(), a) < std::find(order.begin(), order.end(), b);
                        });
                    }
                    save_config_soon();
                });
        }
        text(cr, area.x, area.y + 54, "Estilo", {ui(12), p.text_dim, Align::Left});
        const char* styles[2] = {"Números", "Anillos"};
        for (int k = 0; k < 2; ++k) {
            bool on = (cfg.perf_style == "rings") == (k == 1);
            w::pill(c, ID("set.perf.st", k), {area.x + 86 + k * 106, area.y + 46, 100, 30}, styles[k], nullptr,
                    on ? w::Style::Accent : w::Style::Plain, 11.5f)
                .click([this, k] {
                    config().perf_style = k == 1 ? "rings" : "numbers";
                    save_config_soon();
                });
        }
        text(cr, area.x, area.y + 96, "Cápsulas", {ui(12), p.text_dim, Align::Left});
        const char* split[2] = {"Juntas", "Separadas"};
        for (int k = 0; k < 2; ++k) {
            bool on = cfg.perf_split == (k == 1);
            w::pill(c, ID("set.perf.split", k), {area.x + 86 + k * 106, area.y + 88, 100, 30}, split[k], nullptr,
                    on ? w::Style::Accent : w::Style::Plain, 11.5f)
                .click([this, k] {
                    config().perf_split = k == 1;
                    save_config_soon();
                });
        }
    }
}

void App::draw_modules_editor(cairo_t* cr, Ui& u, const Rect& card) {
    const auto& p = T();
    w::Ctx c{cr, u};
    auto& mods = config().modules;
    sort_modules(mods);
    for (auto& [id, sp] : mod_open_) sp.update(1.0f / 60.0f);
    static const std::map<std::string, std::pair<const char*, const char*>> names = {
        {"workspaces", {"󰕰", "Espacios de trabajo"}}, {"window", {"󰖯", "Ventana activa"}},
        {"search", {"󰍉", "Buscador"}},                {"tray", {"󰀻", "Bandeja del sistema"}},
        {"perf", {"󰓅", "Monitor de rendimiento"}},
        {"notifications", {"󰂚", "Notificaciones"}},   {"network", {"󰤨", "Red"}},
        {"bluetooth", {"󰂯", "Bluetooth (solo si está activo)"}}, {"volume", {"󰕾", "Volumen"}},
        {"battery", {"󰁹", "Batería"}},
    };
    size_t nl = left_count(mods);
    float y = card.y + 58;
    auto heading = [&](const char* t) {
        text(cr, card.x + 18, y + 4, t, {ui(10.5f, "Bold"), p.text_faint, Align::Left});
        y += 24;
    };
    heading("IZQUIERDA");
    if (nl == 0) heading("DERECHA");
    for (size_t i = 0; i < mods.size(); ++i) {
        if (i == nl && nl > 0) heading("DERECHA");
        auto& m = mods[i];
        auto nm = names.count(m.id) ? names.at(m.id) : std::make_pair("󰘔", m.id.c_str());
        float extra = module_extra_h(m.id);
        Rect row{card.x + 12, y, card.w - 24, 40};
        fill(cr, {row.x, row.y, row.w, row.h + extra}, 12, p.text.with_alpha(i % 2 ? 0.03f : 0.05f));
        glyph(cr, row.x + 20, row.cy(), nm.first, 15, m.on ? p.accent : p.text_faint);
        text(cr, row.x + 42, row.cy() - 9, nm.second, {ui(13), m.on ? p.text : p.text_dim, Align::Left, row.w - 330});
        float x = row.right() - 8;
        // order (crossing the edge of the group changes the side)
        x -= 30;
        w::icon_button(c, ID("set.mod.dn", static_cast<int64_t>(i)), {x, row.y + 5, 30, 30}, "󰅀", 14).click([this, i] {
            auto& ms = config().modules;
            size_t n = left_count(ms);
            if (i + 1 == n) ms[i].side = "right";  // last on the left: first on the right
            else if (i + 1 < ms.size()) std::swap(ms[i], ms[i + 1]);
            sort_modules(ms);
            save_config_soon();
        });
        x -= 32;
        w::icon_button(c, ID("set.mod.up", static_cast<int64_t>(i)), {x, row.y + 5, 30, 30}, "󰅃", 14).click([this, i] {
            auto& ms = config().modules;
            size_t n = left_count(ms);
            if (i == n) ms[i].side = "left";  // first on the right: last on the left
            else if (i > 0) std::swap(ms[i], ms[i - 1]);
            sort_modules(ms);
            save_config_soon();
        });
        // side
        x -= 12;
        for (int sd = 1; sd >= 0; --sd) {
            const char* lab = sd ? "Der." : "Izq.";
            x -= 52;
            bool on = (m.side == "right") == (sd == 1);
            w::pill(c, ID("set.mod.side", static_cast<int64_t>(i * 2 + sd)), {x, row.y + 6, 50, 28}, lab, nullptr,
                    on ? w::Style::Accent : w::Style::Plain, 11.5f)
                .click([this, i, sd] {
                    auto& ms = config().modules;
                    std::string side = sd ? "right" : "left";
                    if (ms[i].side == side) return;
                    // to the other group, next to the island: the end of the
                    // left one or the start of the right one
                    ModuleConf mv = ms[i];
                    mv.side = side;
                    ms.erase(ms.begin() + static_cast<long>(i));
                    size_t at = left_count(ms);
                    ms.insert(ms.begin() + static_cast<long>(at), mv);
                    save_config_soon();
                });
            x -= 2;
        }
        // on/off
        x -= 58;
        w::toggle(c, ID("set.mod.on", static_cast<int64_t>(i)), {x, row.y + 7, 46, 26}, m.on, [this, i] {
            config().modules[i].on = !config().modules[i].on;
            save_config_soon();
        });
        // its options, folded under a gear: the card grows to show them
        if (module_options_h(m.id) > 0) {
            x -= 40;
            Spring& sp = mod_open_[m.id];
            bool open = sp.target > 0.5f;
            std::string mid = m.id;
            w::icon_button(c, ID("set.mod.gear", static_cast<int64_t>(i)), {x, row.y + 5, 30, 30}, "󰒓", 14, open)
                .click([this, mid] {
                    Spring& s2 = mod_open_[mid];
                    s2.set(s2.target > 0.5f ? 0.0f : 1.0f);
                    settings_.request();
                });
            if (extra > 0.5f) {
                Rect area{row.x + 42, row.bottom(), row.w - 54, extra};
                cairo_save(cr);
                cairo_rectangle(cr, area.x - 4, area.y, area.w + 8, area.h);
                cairo_clip(cr);
                u.push_clip(area);
                float a = clamp01(extra / module_options_h(m.id));
                cairo_push_group(cr);
                draw_module_options(cr, u, m.id, {area.x, area.y, area.w, module_options_h(m.id)});
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, a);
                u.pop_clip();
                cairo_restore(cr);
            }
        }
        y += 44 + extra;
    }
    if (nl == mods.size()) heading("DERECHA");
}

// The shadow pad: a dial instead of four sliders.
void App::draw_shadow_pad(cairo_t* cr, Ui& u, const Rect& card) {
    const auto& p = T();
    auto& cfg = config();
    float cx = card.x + 150, cy = card.y + 132, R = 58, ring = R + 16;
    float k = clamp01(cfg.shadow_distance / 24.0f);
    float a = cfg.shadow_angle * static_cast<float>(M_PI) / 180.0f;
    bool off = !cfg.shadows;
    // Dial
    cairo_arc(cr, cx, cy, R, 0, 2 * M_PI);
    set(cr, p.text.with_alpha(0.05f));
    cairo_fill(cr);
    for (float rr : {R * 0.33f, R * 0.66f}) {
        cairo_arc(cr, cx, cy, rr, 0, 2 * M_PI);
        set(cr, p.text.with_alpha(0.06f));
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
    divider(cr, cx - R, cy, cx + R, cy, p.text.with_alpha(0.06f));
    divider(cr, cx, cy - R, cx, cy + R, p.text.with_alpha(0.06f));
    // Live preview: a small pill with the real shadow.
    Rect pill{cx - 26, cy - 10, 52, 20};
    if (!off) {
        float alpha = cfg.shadow_opacity, soft = cfg.shadow_softness * 0.5f, d = cfg.shadow_distance * 0.6f;
        cairo_save(cr);
        rrect(cr, pill, 10);
        cairo_rectangle(cr, card.x, card.y, card.w, card.h);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_clip(cr);
        draw_shadow(cr, {pill.x + std::cos(a) * d, pill.y + std::sin(a) * d, pill.w, pill.h}, 10, alpha, std::max(2.0f, soft));
        cairo_restore(cr);
    }
    fill(cr, pill, 10, p.island);
    // Arcs: softness (top) and opacity (bottom).
    auto arc = [&](float a0, float a1, float frac, Color col) {
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 6);
        // a1 < a0 runs the other way round (the opacity arc, at the bottom).
        auto path = [&](float from, float to) {
            if (to >= from) cairo_arc(cr, cx, cy, ring, from, to);
            else cairo_arc_negative(cr, cx, cy, ring, from, to);
        };
        path(a0, a1);
        set(cr, p.text.with_alpha(0.08f));
        cairo_stroke(cr);
        float ae = a0 + (a1 - a0) * clamp01(frac);
        path(a0, ae);
        set(cr, col);
        cairo_stroke(cr);
        fill(cr, {cx + std::cos(ae) * ring - 7, cy + std::sin(ae) * ring - 7, 14, 14}, 7, col);
    };
    const float pi = static_cast<float>(M_PI);
    float soft_frac = (cfg.shadow_softness - 2) / 46.0f;
    arc(pi * 1.12f, pi * 1.88f, soft_frac, off ? p.text_faint : p.accent);
    arc(pi * 0.88f, pi * 0.12f, cfg.shadow_opacity, off ? p.text_faint : mix(p.accent, p.text, 0.35f));
    // The vector and its dot.
    float hx = cx + std::cos(a) * k * R, hy = cy + std::sin(a) * k * R;
    cairo_move_to(cr, cx, cy);
    cairo_line_to(cr, hx, hy);
    set(cr, (off ? p.text_faint : p.accent).with_alpha(0.8f));
    cairo_set_line_width(cr, 2);
    cairo_stroke(cr);
    fill(cr, {hx - 8, hy - 8, 16, 16}, 8, off ? p.text_faint : p.accent);
    fill(cr, {hx - 3, hy - 3, 6, 6}, 3, p.on_accent);

    // Read-out
    float tx = cx + ring + 40;
    auto line = [&](float y, const char* k2, const std::string& v) {
        text(cr, tx, y, k2, {ui(11.5f), p.text_dim});
        text(cr, tx + 110, y, v, {mono(12.5f, "Bold"), p.text});
    };
    line(card.y + 72, "Dirección", std::to_string(static_cast<int>(std::lround(cfg.shadow_angle))) + "°");
    line(card.y + 98, "Distancia", std::to_string(static_cast<int>(std::lround(cfg.shadow_distance))) + " px");
    line(card.y + 124, "Suavidad", std::to_string(static_cast<int>(std::lround(cfg.shadow_softness))) + " px");
    line(card.y + 150, "Opacidad", std::to_string(static_cast<int>(std::lround(cfg.shadow_opacity * 100))) + "%");
    w::Ctx wc{cr, u};
    w::pill(wc, ID("set.shreset"), {tx, card.y + 176, 130, 28}, "Restablecer", "󰑓", w::Style::Plain, 11.5f)
        .click([this] {
            auto& c = config();
            c.shadow_opacity = 0.40f;
            c.shadow_softness = 18;
            c.shadow_distance = 4;
            c.shadow_angle = 90;
            save_config_soon();
        });

    // One region: the drag decides from where it started what it moves.
    Rect area{cx - ring - 12, cy - ring - 12, 2 * ring + 24, 2 * ring + 24};
    u.add(ID("set.shpad"), area)
        .drag([this, cx, cy, R, ring](const DragEvent& e) {
            auto& c = config();
            float dx = e.lx - cx, dy = e.ly - cy, d = std::sqrt(dx * dx + dy * dy);
            if (set_pad_mode_ == 0) set_pad_mode_ = d <= R + 6 ? 1 : dy < 0 ? 2 : 3;
            float ang = std::atan2(dy, dx);
            const float pi = static_cast<float>(M_PI);
            if (set_pad_mode_ == 1) {
                c.shadow_distance = std::round(std::min(1.0f, d / R) * 24);
                float deg = ang * 180 / pi;
                c.shadow_angle = std::round(deg < 0 ? deg + 360 : deg);
            } else {
                // Angle along the arc, as a fraction of its sweep.
                float a = ang < 0 ? ang + 2 * pi : ang;
                if (set_pad_mode_ == 2) {
                    float f = clamp01((a - pi * 1.12f) / (pi * 0.76f));
                    c.shadow_softness = std::round(2 + f * 46);
                } else {
                    float f = clamp01((pi * 0.88f - a) / (pi * 0.76f));
                    c.shadow_opacity = std::round(f * 100) / 100;
                }
            }
            (void)ring;
            save_config_soon();
            settings_.request();
        })
        .release([this](const DragEvent&) { set_pad_mode_ = 0; })
        .scroll([this](float dy) {
            auto& c = config();
            c.shadow_softness = std::clamp(std::round(c.shadow_softness - dy * 2), 2.0f, 48.0f);
            save_config_soon();
            settings_.request();
        });
}

void App::save_config_soon() {
    loop_.cancel(set_save_timer_);
    set_save_timer_ = loop_.after(0.2, [this] {
        set_save_timer_ = 0;
        make_dirs(config_dir());
        write_file_atomic(Config::path(), config().to_json());
    });
}

void App::open_settings() {
    if (!config().cmd_settings.empty() && config().cmd_settings != "internal") {
        proc::spawn_shell(config().cmd_settings);
        return;
    }
    close_all();
    set_search_.clear();
    set_edit_key_.clear();
    set_scroll_.reset(0);
    settings_.open();
}

void App::settings_key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    TextField* f = set_edit_key_.empty() ? &set_search_ : &set_edit_;
    auto r = f->key(sym, utf8, mods);
    if (r == TextField::Result::Cancel) {
        if (!set_edit_key_.empty()) set_edit_key_.clear();
        else if (!set_search_.text.empty()) set_search_.clear();
        else settings_.close_now();
    } else if (r == TextField::Result::Submit && !set_edit_key_.empty()) {
        set_edit_commit_ = true;
    } else if (r == TextField::Result::Changed && f == &set_search_) {
        set_scroll_.reset(0);
    }
    settings_.request();
}

void App::draw_settings(cairo_t* cr, Ui& u, float w, float h, float dt) {
    (void)dt;
    const auto& p = T();
    auto& cfg = config();
    w::Ctx c{cr, u};
    // (the fade and its zoom are the overlay's, on a snapshot)

    // Backdrop
    cairo_set_source_rgba(cr, 0, 0, 0, 0.38);
    cairo_paint(cr);

    float W = std::min(1060.0f, w - 80), H = std::min(700.0f, h - 80);
    Rect win{std::round((w - W) / 2), std::round((h - H) / 2), W, H};
    cairo_save(cr);
    {
        // Cached nine-slice shadow, outside the window only.
        cairo_save(cr);
        rrect(cr, win, 26);
        cairo_rectangle(cr, 0, 0, w, h);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_clip(cr);
        draw_shadow(cr, {win.x, win.y + 8, win.w, win.h}, 26, 0.55f, 30);
        cairo_restore(cr);
    }
    fill(cr, win, 26, glass_body().with_alpha(std::max(0.9f, glass_body().a)));
    u.add(ID("set.win"), win);  // clicks inside never reach the backdrop
    if (cfg.glass && cfg.compositor_blur && wl_.can_blur() && settings_.is_open()) settings_.blur({{win, 26, false}});
    else settings_.blur({});

    // ── rows ────────────────────────────────────────────────────────
    std::vector<SetRow> rows;
    auto toggle = [&](int sec, const std::string& grp, const std::string& t, const std::string& s, std::function<bool()> g,
                      std::function<void(bool)> st) {
        SetRow r;
        r.kind = SetRow::Toggle;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.get_b = g, r.set_b = st;
        rows.push_back(std::move(r));
    };
    auto choice = [&](int sec, const std::string& grp, const std::string& t, const std::string& s,
                      std::vector<std::string> opts, std::function<int()> g, std::function<void(int)> st) {
        SetRow r;
        r.kind = SetRow::Choice;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.options = std::move(opts), r.get_i = g, r.set_i = st;
        rows.push_back(std::move(r));
    };
    auto slider = [&](int sec, const std::string& grp, const std::string& t, const std::string& s, float lo, float hi,
                      std::function<float()> g, std::function<void(float)> st, std::function<std::string(float)> fmt) {
        SetRow r;
        r.kind = SetRow::Slider;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.lo = lo, r.hi = hi, r.get_f = g, r.set_f = st, r.fmt = fmt;
        rows.push_back(std::move(r));
    };
    auto field = [&](int sec, const std::string& grp, const std::string& t, const std::string& s, const std::string& key,
                     std::function<std::string()> g, std::function<void(const std::string&)> st) {
        SetRow r;
        r.kind = SetRow::Text;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.key = key, r.get_s = g, r.set_s = st;
        rows.push_back(std::move(r));
    };
    auto info = [&](int sec, const std::string& grp, const std::string& t, const std::string& s) {
        SetRow r;
        r.kind = SetRow::Info;
        r.section = sec, r.group = grp, r.title = t, r.sub = s;
        rows.push_back(std::move(r));
    };
    auto buttons = [&](int sec, const std::string& grp, const std::string& t, const std::string& s,
                       std::vector<std::pair<std::string, std::function<void()>>> b) {
        SetRow r;
        r.kind = SetRow::Buttons;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.buttons = std::move(b);
        rows.push_back(std::move(r));
    };
    auto preview = [&](int sec, const std::string& grp, const std::string& t, const std::string& s,
                       std::vector<std::string> opts, std::function<int()> g, std::function<void(int)> st,
                       std::function<void(cairo_t*, const Rect&, int, float)> pv) {
        SetRow r;
        r.kind = SetRow::Preview;
        r.section = sec, r.group = grp, r.title = t, r.sub = s, r.options = std::move(opts), r.get_i = g, r.set_i = st,
        r.preview = pv;
        rows.push_back(std::move(r));
    };
    auto save = [this] { save_config_soon(); };
    auto pct = [](float v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; };

    // Apariencia
    static const std::vector<std::string> themes = {"adaptive", "lavender", "ocean", "rose", "mint", "amber", "mono"};
    choice(0, "Color", "Tema", "Adaptativo toma la paleta de tu fondo de pantalla",
           {"Adaptativo", "Lavanda", "Océano", "Rosa", "Menta", "Ámbar", "Mono"},
           [&] {
               auto it = std::find(themes.begin(), themes.end(), cfg.theme);
               return it == themes.end() ? 0 : static_cast<int>(it - themes.begin());
           },
           [&, save](int i) {
               cfg.theme = themes[i];
               save();
           });
    toggle(0, "Reloj", "Formato de 24 horas", "15:30 en lugar de 3:30", [&] { return cfg.clock_24h; },
           [&, save](bool v) {
               cfg.clock_24h = v;
               save();
           });
    slider(0, "Movimiento", "Velocidad de animaciones", "0 desactiva el movimiento", 0, 2,
           [&] { return cfg.motion; },
           [&, save](float v) {
               cfg.motion = std::round(v * 20) / 20;
               Spring::speed = cfg.motion;
               save();
           },
           [](float v) {
               char b[16];
               snprintf(b, sizeof(b), "%.2f×", v);
               return std::string(b);
           });
    slider(0, "Movimiento", "Espera del resumen", "Cuánto hay que dejar el cursor sobre la isla", 0, 600,
           [&] { return static_cast<float>(cfg.glance_delay_ms); },
           [&, save](float v) {
               cfg.glance_delay_ms = static_cast<int>(std::lround(v / 10) * 10);
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + " ms"; });

    // Barra e isla
    static const std::vector<std::string> layouts = {"grouped", "spread", "island"};
    preview(1, "Distribución", "Barra superior", "", {"Agrupada", "Extendida", "Una isla"},
            [&] {
                auto it = std::find(layouts.begin(), layouts.end(), cfg.bar_layout);
                return it == layouts.end() ? 0 : static_cast<int>(it - layouts.begin());
            },
            [&, save](int i) {
                cfg.bar_layout = layouts[i];
                save();
            },
            preview_layout);
    preview(1, "Isla", "Estilo de la isla", "", {"Flotante", "Acoplada al borde"},
            [&] { return cfg.island_style == "attached" ? 1 : 0; },
            [&, save](int i) {
                cfg.island_style = i == 1 ? "attached" : "floating";
                save();
            },
            preview_style);
    toggle(1, "Isla", "Acoplar también las laterales", "Con la isla acoplada: desactivado, solo la central se pega al borde",
           [&] { return cfg.dock_sides; },
           [&, save](bool v) {
               cfg.dock_sides = v;
               save();
           });
    slider(1, "Isla", "Altura de las píldoras", "Tamaño vertical de la isla en reposo y de las laterales", 24, 48,
           [&] { return static_cast<float>(cfg.bar_height); },
           [&, save](float v) {
               cfg.bar_height = static_cast<int>(std::lround(v));
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + " px"; });
    slider(1, "Isla", "Redondez", "De casi plana a completamente redonda", 0, 1, [&] { return cfg.roundness; },
           [&, save](float v) {
               cfg.roundness = std::round(v * 100) / 100;
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v * 100))) + "%"; });
    toggle(1, "Sombras", "Sombras", "Bajo la isla, sus píldoras y los paneles", [&] { return cfg.shadows; },
           [&, save](bool v) {
               cfg.shadows = v;
               save();
           });
    {
        SetRow r;
        r.kind = SetRow::Custom;
        r.section = 1, r.group = "Sombras", r.title = "Dirección, distancia y suavidad",
        r.sub = "Punto: dirección y distancia · arco de arriba: suavidad · arco de abajo: opacidad · rueda: suavidad";
        r.custom_h = 216;
        r.custom = [this, &u](cairo_t* c2, const Rect& card) { draw_shadow_pad(c2, u, card); };
        rows.push_back(std::move(r));
    }
    {
        SetRow r;
        r.kind = SetRow::Custom;
        r.section = 1, r.group = "Módulos", r.title = "Módulos de la barra",
        r.sub = "Actívalos, elige su lado y su orden: la lista es la barra, de izquierda a derecha. La tuerca abre sus opciones";
        r.custom_h = 64 + 44.0f * cfg.modules.size() + 48;
        for (auto& m : cfg.modules) r.custom_h += module_extra_h(m.id);
        for (auto& m : cfg.modules) r.keywords += " " + module_option_words(m.id);
        r.custom = [this, &u](cairo_t* c2, const Rect& card) { draw_modules_editor(c2, u, card); };
        rows.push_back(std::move(r));
    }
    toggle(1, "Espacio", "Reservar espacio", "Las ventanas no se colocan debajo de la isla", [&] { return cfg.reserve_space; },
           [&, save](bool v) {
               cfg.reserve_space = v;
               save();
           });
    slider(1, "Espacio", "Espacio con las ventanas", "Separación entre la isla y las ventanas de abajo (0 = pegadas)", 0, 40,
           [&] { return static_cast<float>(cfg.window_gap); },
           [&, save](float v) {
               cfg.window_gap = static_cast<int>(std::lround(v));
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + " px"; });
    slider(1, "Espacio", "Margen superior", "Separación entre la isla flotante y el borde", 0, 40,
           [&] { return static_cast<float>(cfg.margin_top); },
           [&, save](float v) {
               cfg.margin_top = static_cast<int>(std::lround(v));
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + " px"; });

    // Cristal
    toggle(2, "Cristal", "Transparencia", "La isla deja ver el escritorio desenfocado detrás", [&] { return cfg.glass; },
           [&, save](bool v) {
               cfg.glass = v;
               save();
           });
    slider(2, "Cristal", "Opacidad", "Más baja, más cristal", 0.3f, 1.0f, [&] { return cfg.island_opacity; },
           [&, save](float v) {
               cfg.island_opacity = std::round(v * 100) / 100;
               save();
           },
           pct);
    toggle(2, "Compositor", "Pedir desenfoque a niri",
           wl_.can_blur() ? "Usa los parámetros de blur de tu config de niri (grano, xray…)"
                          : "Tu compositor no ofrece desenfoque",
           [&] { return cfg.compositor_blur; },
           [&, save](bool v) {
               cfg.compositor_blur = v;
               save();
           });
    toggle(2, "Compositor", "Rayos X (xray)",
           "Activado: detrás solo se ve el fondo · Desactivado: también se desenfocan las ventanas",
           [&] { return cfg.blur_xray; },
           [&, save](bool v) {
               cfg.blur_xray = v;
               save();
           });

    // Notificaciones
    choice(3, "No molestar", "Modo", "Discreto: suena y el icono vibra junto a la isla · Total: nada",
           {"Desactivado", "Discreto", "Total"}, [this] { return notifs_.dnd_mode(); },
           [this](int i) { notifs_.set_dnd_mode(i); });
    slider(3, "Ventana emergente", "Duración", "Tiempo que se queda en la isla", 2000, 15000,
           [&] { return static_cast<float>(cfg.notification_timeout_ms); },
           [&, save](float v) {
               cfg.notification_timeout_ms = static_cast<int>(std::lround(v / 500) * 500);
               save();
           },
           [](float v) {
               char b[16];
               snprintf(b, sizeof(b), "%.1f s", v / 1000);
               return std::string(b);
           });
    slider(3, "Historial", "Notificaciones guardadas", "", 10, 200,
           [&] { return static_cast<float>(cfg.notification_history); },
           [&, save](float v) {
               cfg.notification_history = static_cast<int>(std::lround(v / 5) * 5);
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))); });
    static const std::vector<std::string> modes = {"auto", "server", "monitor", "off"};
    choice(3, "Servicio", "Recepción", "Automático se hace servidor si nadie más lo es (se aplica al reiniciar)",
           {"Automático", "Servidor", "Observador", "Apagado"},
           [&] {
               auto it = std::find(modes.begin(), modes.end(), cfg.notification_mode);
               return it == modes.end() ? 0 : static_cast<int>(it - modes.begin());
           },
           [&, save](int i) {
               cfg.notification_mode = modes[i];
               save();
           });

    // Capturas y grabación
    auto& ro = recorder_.options();
    toggle(4, "Capturas", "Copiar al portapapeles", "Cada captura guardada también se copia", [&] { return cfg.screenshot_copy; },
           [&, save](bool v) {
               cfg.screenshot_copy = v;
               save();
           });
    field(4, "Capturas", "Carpeta de capturas", "Vacío: Imágenes/Capturas", "shots", [&] { return cfg.screenshot_dir; },
          [&, save](const std::string& v) {
              cfg.screenshot_dir = v;
              save();
          });
    field(4, "Grabación", "Carpeta de grabaciones", "Vacío: Vídeos/Grabaciones", "recs", [&] { return cfg.recording_dir; },
          [&, save](const std::string& v) {
              cfg.recording_dir = v;
              save();
          });
    choice(4, "Grabación", "Fotogramas", "", {"30 fps", "60 fps"}, [&ro] { return ro.fps == 30 ? 0 : 1; },
           [this, &ro](int i) {
               ro.fps = i == 0 ? 30 : 60;
               recorder_.save_options();
           });
    choice(4, "Grabación", "Calidad", "", {"Alta", "Equilibrada", "Ligera"}, [&ro] { return ro.quality; },
           [this, &ro](int i) {
               ro.quality = i;
               recorder_.save_options();
           });
    choice(4, "Grabación", "Codificador", "GPU usa VA-API y vuelve a CPU si falla", {"GPU", "CPU"},
           [&ro] { return ro.encoder; },
           [this, &ro](int i) {
               ro.encoder = i;
               recorder_.save_options();
           });
    choice(4, "Grabación", "Formato", "", {"MP4", "MKV", "GIF"}, [&ro] { return ro.format; },
           [this, &ro](int i) {
               ro.format = i;
               recorder_.save_options();
           });
    choice(4, "Grabación", "Cuenta atrás", "", {"No", "3 s", "5 s", "10 s"},
           [&ro] { return ro.countdown == 0 ? 0 : ro.countdown <= 3 ? 1 : ro.countdown <= 5 ? 2 : 3; },
           [this, &ro](int i) {
               static const int v[] = {0, 3, 5, 10};
               ro.countdown = v[i];
               recorder_.save_options();
           });

    // Fondos
    field(5, "Carpetas", "Carpetas de fondos", "Separadas por comas", "walldirs",
          [&] {
              std::string all;
              for (auto& d : cfg.wallpaper_dirs) all += (all.empty() ? "" : ", ") + d;
              return all;
          },
          [&, save](const std::string& v) {
              cfg.wallpaper_dirs.clear();
              for (auto& d : split(v, ','))
                  if (!trim(d).empty()) cfg.wallpaper_dirs.push_back(trim(d));
              save();
          });
    field(5, "Aplicar", "Comando para aplicar", "{path} es la imagen elegida", "wallapply",
          [&] { return cfg.cmd_wallpaper_apply; },
          [&, save](const std::string& v) {
              cfg.cmd_wallpaper_apply = v;
              save();
          });
    toggle(5, "Vídeo", "Pausar con pantalla completa", "Los fondos animados se detienen mientras una ventana ocupa toda la pantalla",
           [&] { return cfg.wallpaper_pause_covered; },
           [&, save](bool v) {
               cfg.wallpaper_pause_covered = v;
               save();
           });
    // Wallpaper Engine
    toggle(5, "Wallpaper Engine", "Wallpaper Engine", "Los fondos del Workshop que tengas descargados aparecen en el selector",
           [&] { return cfg.wallpaper_engine; },
           [&, save](bool v) {
               cfg.wallpaper_engine = v;
               save();
           });
    toggle(5, "Wallpaper Engine", "Sonido", "Los fondos con audio suenan",
           [&] { return cfg.wallpaper_engine_audio; },
           [&, save](bool v) {
               cfg.wallpaper_engine_audio = v;
               save();
               backdrop_.set_audio(v);
           });
    toggle(5, "Wallpaper Engine", "Borrar los que dejas de seguir", "Steam deja en disco los fondos de los que te desuscribes; la isla los borra",
           [&] { return cfg.wallpaper_engine_clean; },
           [&, save](bool v) {
               cfg.wallpaper_engine_clean = v;
               save();
           });
    buttons(5, "Wallpaper Engine", "Más fondos", "El Workshop también está en el selector; los fondos web necesitan su soporte",
            {{"Abrir Workshop", [] { proc::spawn({"xdg-open", "steam://url/SteamWorkshopPage/431960"}); }},
             {WebWall::available() ? "Reinstalar soporte web" : "Instalar soporte web", [this] {
                  show_toast("Descargando el soporte web…");
                  auto err = std::make_shared<std::string>();
                  auto ok = std::make_shared<bool>(false);
                  Loop::get().async([err, ok] { *ok = WebWall::install_renderer(err.get()); },
                                    [this, err, ok] { show_toast(*ok ? "Soporte web listo" : *err); });
              }}});
    preview(5, "Selector", "Estilo del selector", "", {"Carrusel", "Panal", "Tarjetas"},
            [&] {
                const std::string& st = cfg.wallpaper_picker_style;
                return st == "cards" ? 2 : (st == "hex" || st == "grid") ? 1 : 0;
            },
            [&, save](int i) {
                static const char* styles[3] = {"slices", "hex", "cards"};
                cfg.wallpaper_picker_style = styles[i];
                save();
            },
            preview_picker);
    buttons(5, "Selector", "Selector de fondos", "", {{"Abrir selector", [this] {
                                                           settings_.close_now();
                                                           open_wallpaper_picker();
                                                       }}});

    // Sistema
    // The bubbles screensaver
    toggle(6, "Protector de pantalla", "Protector de pantalla", "Burbujas cuando no usas el equipo un rato",
           [&] { return cfg.screensaver; },
           [&, save](bool v) {
               cfg.screensaver = v;
               save();
           });
    slider(6, "Protector de pantalla", "Esperar", "Tiempo sin usar el equipo antes de que aparezca", 1, 30,
           [&] { return static_cast<float>(cfg.screensaver_minutes); },
           [&, save](float v) {
               cfg.screensaver_minutes = static_cast<int>(std::lround(v));
               save();
           },
           [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + " min"; });
    slider(6, "Protector de pantalla", "Bloquear también", "Sin usar el equipo este tiempo, se bloquea (0 = nunca)", 0, 60,
           [&] { return static_cast<float>(cfg.screensaver_lock_minutes); },
           [&, save](float v) {
               cfg.screensaver_lock_minutes = static_cast<int>(std::lround(v));
               save();
           },
           [](float v) {
               int m = static_cast<int>(std::lround(v));
               return m == 0 ? std::string("Nunca") : std::to_string(m) + " min";
           });
    choice(6, "Protector de pantalla", "Burbujas", "Cuántas salen a la vez", {"Pocas", "Normal", "Muchas"},
           [&] { return std::clamp(cfg.screensaver_bubbles, 0, 2); },
           [&, save](int i) {
               cfg.screensaver_bubbles = i;
               save();
           });
    slider(6, "Protector de pantalla", "Velocidad", "De flotar despacio a ir con prisa", 0.5f, 2.0f,
           [&] { return cfg.screensaver_speed; },
           [&, save](float v) {
               cfg.screensaver_speed = std::round(v * 10) / 10;
               save();
           },
           [](float v) {
               char b[16];
               snprintf(b, sizeof(b), "×%.1f", v);
               return std::string(b);
           });
    buttons(6, "Protector de pantalla", "Probarlo", "Mueve el ratón o pulsa una tecla para salir",
            {{"Probar ahora", [this] {
                  settings_.close_now();
                  loop_.after(0.4, [this] { start_screensaver(); });
              }}});

    field(6, "Aplicaciones", "Terminal", "", "term", [&] { return cfg.terminal; },
          [&, save](const std::string& v) {
              cfg.terminal = v;
              save();
          });
    field(6, "Sesión", "Bloquear pantalla", "Comando de bloqueo", "lock", [&] { return cfg.cmd_lock; },
          [&, save](const std::string& v) {
              cfg.cmd_lock = v;
              save();
          });
    field(6, "Sesión", "Luz nocturna", "Comando para alternarla", "night", [&] { return cfg.cmd_night_light; },
          [&, save](const std::string& v) {
              cfg.cmd_night_light = v;
              save();
          });
    field(6, "Tipografía", "Fuente de la interfaz", "", "font", [&] { return cfg.font; },
          [&, save](const std::string& v) {
              cfg.font = v;
              save();
          });
    field(6, "Tipografía", "Fuente de las notas", "Manuscrita, por ejemplo Caveat", "notefont", [&] { return cfg.note_font; },
          [&, save](const std::string& v) {
              cfg.note_font = v;
              save();
          });

    // Acerca de
    info(7, "niri-island", "Versión 2.0", "Isla dinámica para niri, en C++ (cairo + pango + layer-shell)");
    info(7, "Archivo", "Configuración", Config::path());
    buttons(7, "Archivo", "Acciones", "",
            {{"Abrir config.json", [this] { proc::spawn({"xdg-open", Config::path()}); }},
             {"Recargar", [this] { command("reload"); }}});

    // ── sidebar ─────────────────────────────────────────────────────
    float sbw = 248;
    Rect sb{win.x, win.y, sbw, win.h};
    divider(cr, sb.right(), win.y + 18, sb.right(), win.bottom() - 18, p.border);
    text(cr, sb.x + 24, sb.y + 22, "Ajustes", {ui(20, "Bold"), p.text});
    Rect sf{sb.x + 16, sb.y + 62, sbw - 32, 36};
    bool searching = set_edit_key_.empty();
    fill(cr, sf, 18, p.text.with_alpha(0.06f));
    glyph(cr, sf.x + 18, sf.cy(), "󰍉", 14, p.text_dim);
    Text st{ui(13), p.text};
    if (set_search_.text.empty()) text(cr, sf.x + 34, sf.cy() - 9, "Buscar ajustes…", {ui(13), p.text_faint});
    else text(cr, sf.x + 34, sf.cy() - 9, set_search_.text, {ui(13), p.text, Align::Left, sf.w - 46});
    if (searching && std::fmod(anim_time_, 1.1f) < 0.7f)
        fill(cr, {sf.x + 34 + caret_x(set_search_.text, st, set_search_.caret), sf.cy() - 9, 2, 18}, 1, p.accent);
    u.add(ID("set.search"), sf).cursor_shape(CursorShape::Text).click([this] { set_edit_key_.clear(); });

    float sy = sb.y + 114;
    for (int i = 0; i < kSectionCount; ++i) {
        Rect r{sb.x + 12, sy + i * 42.0f, sbw - 24, 38};
        uint64_t id = ID("set.sec", i);
        float hv = u.hover(id);
        bool on = set_section_ == i && set_search_.text.empty();
        if (on) fill(cr, r, 12, p.accent_dim);
        else if (hv > 0.01f) fill(cr, r, 12, p.text.with_alpha(0.06f * hv));
        glyph(cr, r.x + 20, r.cy(), kSections[i].glyph, 15, on ? p.accent : p.text_dim);
        text(cr, r.x + 42, r.cy() - 9, kSections[i].title, {ui(13, on ? "SemiBold" : ""), on ? p.text : p.text_dim});
        u.add(id, r).click([this, i] {
            set_section_ = i;
            set_search_.clear();
            set_scroll_.reset(0);
        });
    }

    // ── content ─────────────────────────────────────────────────────
    Rect ca{sb.right() + 28, win.y + 24, win.right() - sb.right() - 56, win.h - 48};
    // accents don't matter: "modulos" finds "Módulos"
    auto fold = [](std::string t) {
        t = lower(t);
        static const std::pair<const char*, const char*> map[] = {{"á", "a"}, {"é", "e"}, {"í", "i"}, {"ó", "o"}, {"ú", "u"},
                                                                  {"Á", "a"}, {"É", "e"}, {"Í", "i"}, {"Ó", "o"}, {"Ú", "u"},
                                                                  {"ñ", "n"}, {"ü", "u"}};
        for (auto& [a, b] : map) t = replace_all(t, a, b);
        return t;
    };
    std::string q = fold(set_search_.text);
    std::vector<const SetRow*> shown;
    for (auto& r : rows) {
        if (q.empty() ? r.section == set_section_
                      : fold(r.title + " " + r.sub + " " + r.group + " " + r.keywords).find(q) != std::string::npos)
            shown.push_back(&r);
    }
    // A search that names a module's option opens that module's card (and
    // closes it again once the search no longer does).
    for (auto& m : config().modules) {
        std::string words = module_option_words(m.id);
        if (words.empty()) continue;
        bool hit = !q.empty() && fold(words).find(q) != std::string::npos;
        bool was = mod_auto_open_.count(m.id) > 0;
        if (hit && !was) {
            mod_auto_open_.insert(m.id);
            mod_open_[m.id].set(1);
        } else if (!hit && was) {
            mod_auto_open_.erase(m.id);
            mod_open_[m.id].set(0);
        }
    }
    if (q.empty()) {
        text(cr, ca.x, ca.y, kSections[set_section_].title, {ui(22, "Bold"), p.text});
        text(cr, ca.x, ca.y + 32, kSections[set_section_].about, {ui(12.5f), p.text_dim});
    } else {
        text(cr, ca.x, ca.y, "Resultados", {ui(22, "Bold"), p.text});
        text(cr, ca.x, ca.y + 32, std::to_string(shown.size()) + " ajustes para «" + set_search_.text + "»",
             {ui(12.5f), p.text_dim});
    }
    Rect area{ca.x - 6, ca.y + 64, ca.w + 12, ca.h - 64};
    // The scroll catcher goes under the rows (it would swallow their clicks on top).
    float last_content = set_content_;
    u.add(ID("set.area"), area).scroll([this, last_content, area](float dy) {
        set_scroll_.set(std::clamp(set_scroll_.target + dy * 70, 0.0f, std::max(0.0f, last_content - area.h)));
        settings_.request();
    });
    cairo_save(cr);
    cairo_rectangle(cr, area.x, area.y, area.w, area.h);
    cairo_clip(cr);
    u.push_clip(area);
    float y = area.y - set_scroll_.value;
    std::string last_group;
    int ri = 0;
    for (const SetRow* rp : shown) {
        const SetRow& r = *rp;
        std::string grp = q.empty() ? r.group : std::string(kSections[r.section].title) + " · " + r.group;
        if (grp != last_group) {
            y += last_group.empty() ? 0 : 10;
            text(cr, ca.x + 4, y, grp, {ui(11, "Bold"), p.text_faint});
            y += 22;
            last_group = grp;
        }
        // Choices too wide for the right side go under the title.
        float ctl_w = 0;
        if (r.kind == SetRow::Choice)
            for (auto& o : r.options) ctl_w += measure(o, {ui(12, "SemiBold")}).w + 26;
        bool stacked = r.kind == SetRow::Choice && ctl_w > ca.w * 0.55f;
        float rh = r.kind == SetRow::Custom ? r.custom_h : r.kind == SetRow::Preview ? 168 : stacked ? 100 : 64;
        Rect card{ca.x, y, ca.w, rh};
        if (card.bottom() > area.y - 10 && card.y < area.bottom()) {
            fill(cr, card, 16, mix(p.surface, p.surface_hi, 0.25f));
            float ty = r.sub.empty() || r.kind == SetRow::Preview ? card.y + (r.kind == SetRow::Preview ? 14 : 22) : card.y + 13;
            if (r.kind == SetRow::Custom) ty = card.y + 16;
            text(cr, card.x + 18, ty, r.title, {ui(13.5f, "SemiBold"), p.text, Align::Left, card.w * 0.5f});
            if (!r.sub.empty() && r.kind != SetRow::Preview)
                text(cr, card.x + 18, card.y + 33, r.sub,
                     {ui(11.5f), p.text_dim, Align::Left,
                      stacked || r.kind == SetRow::Info || r.kind == SetRow::Custom ? card.w - 36 : card.w * 0.5f});
            Rect ctl{card.right() - 18, card.y, 0, rh};
            switch (r.kind) {
                case SetRow::Toggle: {
                    bool v = r.get_b();
                    auto setter = r.set_b;
                    w::toggle(c, ID("set.tg", ri), {card.right() - 18 - 46, card.cy() - 13, 46, 26}, v,
                              [setter, v] { setter(!v); });
                    break;
                }
                case SetRow::Choice: {
                    int cur = r.get_i();
                    float x = stacked ? card.x + 14 : card.right() - 14 - ctl_w;
                    float cy = stacked ? card.y + 56 : card.y + 16;
                    for (size_t k = 0; k < r.options.size(); ++k) {
                        float ow = measure(r.options[k], {ui(12, "SemiBold")}).w + 22;
                        auto setter = r.set_i;
                        int kk = static_cast<int>(k);
                        w::pill(c, ID("set.ch", ri * 16 + kk), {x, cy, ow, 32}, r.options[k], nullptr,
                                kk == cur ? w::Style::Accent : w::Style::Plain, 12)
                            .click([setter, kk] { setter(kk); });
                        x += ow + 4;
                    }
                    break;
                }
                case SetRow::Slider: {
                    float v = r.get_f();
                    float t = (v - r.lo) / (r.hi - r.lo);
                    float lo = r.lo, hi = r.hi;
                    auto setter = r.set_f;
                    w::slider(c, ID("set.sl", ri), {card.right() - 18 - 300, card.cy() - 18, 300, 36}, clamp01(t), nullptr,
                              [setter, lo, hi](float f) { setter(lo + f * (hi - lo)); }, {}, r.fmt ? r.fmt(v) : "");
                    break;
                }
                case SetRow::Text: {
                    bool editing = set_edit_key_ == r.key;
                    if (editing && set_edit_commit_) {
                        set_edit_commit_ = false;
                        r.set_s(set_edit_.text);
                        set_edit_key_.clear();
                        editing = false;
                    }
                    Rect fr{card.right() - 18 - 340, card.cy() - 17, 340, 34};
                    uint64_t id = ID("set.tx", ri);
                    float hv = u.hover(id);
                    fill(cr, fr, 12, p.text.with_alpha(editing ? 0.1f : 0.05f + 0.03f * hv));
                    if (editing) stroke(cr, fr, 12, p.accent, 1.5f);
                    std::string val = editing ? set_edit_.text : r.get_s();
                    Text ft{mono(12), val.empty() && !editing ? p.text_faint : p.text, Align::Left, fr.w - 24};
                    text(cr, fr.x + 12, fr.cy() - 8, val.empty() && !editing ? "(vacío)" : val, ft);
                    if (editing && std::fmod(anim_time_, 1.1f) < 0.7f)
                        fill(cr, {fr.x + 12 + std::min(fr.w - 24, caret_x(set_edit_.text, ft, set_edit_.caret)), fr.cy() - 9, 2, 18},
                             1, p.accent);
                    std::string key = r.key;
                    auto getter = r.get_s;
                    u.add(id, fr).cursor_shape(CursorShape::Text).click([this, key, getter] {
                        set_edit_key_ = key;
                        set_edit_.set(getter());
                    });
                    if (editing)
                        text(cr, fr.x, fr.bottom() + 3, "Intro guarda · Esc cancela", {ui(10), p.text_faint});
                    break;
                }
                case SetRow::Buttons: {
                    float x = card.right() - 18;
                    for (size_t k = r.buttons.size(); k-- > 0;) {
                        float bw = measure(r.buttons[k].first, {ui(12, "SemiBold")}).w + 30;
                        x -= bw;
                        auto fn = r.buttons[k].second;
                        w::pill(c, ID("set.bt", ri * 8 + static_cast<int>(k)), {x, card.cy() - 16, bw, 32},
                                r.buttons[k].first, nullptr, k == 0 ? w::Style::Accent : w::Style::Tonal, 12)
                            .click(fn);
                        x -= 8;
                    }
                    break;
                }
                case SetRow::Preview: {
                    int cur = r.get_i();
                    size_t n = r.options.size();
                    float pw = std::min(190.0f, (card.w - 36 - (n - 1) * 12) / n);
                    for (size_t k = 0; k < n; ++k) {
                        Rect pr{card.x + 18 + k * (pw + 12), card.y + 42, pw, 96};
                        uint64_t id = ID("set.pv", ri * 8 + static_cast<int>(k));
                        float hv = u.hover(id);
                        bool on = static_cast<int>(k) == cur;
                        r.preview(cr, {pr.x, pr.y, pr.w, 80}, static_cast<int>(k), hv);
                        if (on) stroke(cr, Rect{pr.x, pr.y, pr.w, 80}.inset(-2.5f), 14, p.accent, 2);
                        else if (hv > 0.01f) stroke(cr, Rect{pr.x, pr.y, pr.w, 80}.inset(-2), 14, p.text.with_alpha(0.25f * hv), 1.5f);
                        text(cr, pr.cx(), pr.y + 86, r.options[k], {ui(12, on ? "SemiBold" : ""), on ? p.accent : p.text_dim, Align::Center});
                        auto setter = r.set_i;
                        int kk = static_cast<int>(k);
                        u.add(id, pr).click([setter, kk] { setter(kk); });
                    }
                    break;
                }
                case SetRow::Info: break;
                case SetRow::Custom: r.custom(cr, card); break;
            }
            (void)ctl;
        }
        y += rh + 8;
        ++ri;
    }
    if (shown.empty()) text(cr, ca.x + 4, area.y + 10, "Nada coincide con la búsqueda", {ui(13), p.text_dim});
    float content = y + set_scroll_.value - area.y;
    set_content_ = content;
    u.pop_clip();
    cairo_restore(cr);
    set_scroll_.update(dt);
    w::scrollbar(c, {win.right() - 12, area.y + 4, 3, area.h - 8}, set_scroll_.value, content, area.h);

    // Close
    w::icon_button(c, ID("set.close"), {win.right() - 50, win.y + 16, 34, 34}, "󰅖", 15).click([this] { settings_.close_now(); });
    cairo_restore(cr);
}

}  // namespace island#include "services/workshop.hpp"

