// Bar modules and layouts. The same modules (workspaces, the focused app,
// search, tray, notifications and the status ones) are placed three ways,
// like impasto:
//   grouped  capsules beside the island
//   spread   capsules at the two top corners (their own layer surfaces)
//   island   everything inside the resting island
// The island itself floats, or docks to the top edge like a notch.

#include "core/config.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

bool App::attached() const { return config().island_style == "attached"; }

float App::bar_h() const { return static_cast<float>(config().bar_height); }

bool App::sides_docked() const { return attached() && config().dock_sides; }

float App::corner(float h, bool panel) {
    float k = config().roundness;
    float r = panel ? lerp(9.0f, 34.0f, k) : lerp(7.0f, h / 2, k);
    return std::min(r, h / 2);
}

Rect App::cap_geom(float x, float w) const {
    // Next to a docked island, pills that are not docked sit centred on it.
    float h = bar_h();
    if (attached() && !config().dock_sides) return {x, 3, w, h - 6};
    return {x, 0, w, h};
}

Color App::glass_body() const {
    const auto& p = T();
    if (!config().glass) return p.island;
    // Plain transparency; blur and the rest are niri's business.
    return p.island.with_alpha(config().island_opacity);
}

const std::string& App::bar_layout() const {
    static const std::string grouped = "grouped";
    const std::string& l = config().bar_layout;
    return l == "spread" || l == "island" ? l : grouped;
}

// A capsule, or (docked) square on top with concave ears into the edge.
void App::island_outline(cairo_t* cr, const Rect& r, float radius, bool docked) {
    if (!docked) {
        rrect(cr, r, radius);
        return;
    }
    float e = std::min(10.0f, radius);
    float rb = std::min(radius, r.h / 2);
    cairo_new_path(cr);
    cairo_move_to(cr, r.x - e, r.y);
    cairo_arc(cr, r.x - e, r.y + e, e, -M_PI / 2, 0);
    cairo_line_to(cr, r.x, r.bottom() - rb);
    cairo_arc_negative(cr, r.x + rb, r.bottom() - rb, rb, M_PI, M_PI / 2);
    cairo_line_to(cr, r.right() - rb, r.bottom());
    cairo_arc_negative(cr, r.right() - rb, r.bottom() - rb, rb, M_PI / 2, 0);
    cairo_line_to(cr, r.right(), r.y + e);
    cairo_arc(cr, r.right() + e, r.y + e, e, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void App::island_shadow(cairo_t* cr, const Rect& r, float radius, bool docked, float strength) {
    const auto& cfg = config();
    if (!cfg.shadows || cfg.shadow_opacity <= 0.005f) return;
    float alpha = cfg.shadow_opacity * strength;
    // Softer under a see-through body (a dark halo round glass looks odd).
    if (cfg.glass) alpha *= 0.35f + 0.65f * cfg.island_opacity;
    float soft = cfg.shadow_softness * (0.55f + 0.45f * strength);
    float a = cfg.shadow_angle * static_cast<float>(M_PI) / 180.0f;
    float dist = cfg.shadow_distance * (0.6f + 0.4f * strength);
    float dx = std::cos(a) * dist, dy = std::sin(a) * dist;
    // Outside the shape only; docked, from a shape that runs up past the edge.
    cairo_save(cr);
    island_outline(cr, r, radius, docked);
    Rect o = r.inset(-soft * 2 - dist - 16);
    cairo_rectangle(cr, o.x, o.y, o.w, o.h);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    Rect s = docked ? Rect{r.x + dx, r.y - radius * 2, r.w, r.h + radius * 2 + std::max(0.0f, dy)}
                    : Rect{r.x + dx, r.y + dy, r.w, r.h};
    draw_shadow(cr, s, radius, alpha, soft);
    cairo_restore(cr);
}

void App::capsule(cairo_t* cr, const Rect& r) {
    float rad = corner(r.h, false);
    island_shadow(cr, r, rad, sides_docked(), 0.55f);
    island_outline(cr, r, rad, sides_docked());
    set(cr, glass_body());
    cairo_fill(cr);
}

// ── workspaces ─────────────────────────────────────────────────────────────

float App::ws_width() const {
    float n = static_cast<float>(std::max<size_t>(1, niri_.output_workspaces().size()));
    return 22 + 24 + (n - 1) * 14;
}

void App::draw_workspaces(cairo_t* cr, Ui& u, const Rect& r) {
    const auto& p = T();
    auto list = niri_.output_workspaces();
    size_t n = std::max<size_t>(1, list.size());
    float pad = 11, pill = 24, d = 7, step = 14, cy = r.cy();
    float pos = std::clamp(ws_pill_.value, 0.0f, static_cast<float>(n - 1));
    for (size_t j = 0; j < n; ++j) {
        float jf = static_cast<float>(j);
        float dist = std::abs(pos - jf);
        float fade = std::clamp((dist - 0.25f) / 0.5f, 0.0f, 1.0f);
        float shift = std::clamp(jf - pos, 0.0f, 1.0f);
        float dx = r.x + pad + jf * step + shift * (pill - d) + d / 2;
        bool urgent = j < list.size() && list[j].urgent;
        bool occupied = j < list.size() && list[j].active_window != 0;
        Color dc = urgent ? p.bad : occupied ? p.text.with_alpha(0.7f) : p.text.with_alpha(0.32f);
        if (fade > 0.01f) {
            cairo_arc(cr, dx, cy, d / 2, 0, 2 * M_PI);
            set(cr, dc.fade(fade));
            cairo_fill(cr);
        }
        if (j < list.size()) {
            uint64_t wid = list[j].id;
            Rect hr{r.x + pad + jf * step - 3, r.y, step, r.h};
            if (j > static_cast<size_t>(pos)) hr.x += pill - d;
            if (j == static_cast<size_t>(std::lround(pos))) hr.w = pill + 6;
            u.add(ID("ws", static_cast<int64_t>(wid)), hr)
                .click([this, wid] { niri_.focus_workspace(wid); })
                .scroll([this](float dy) { niri_.focus_workspace_relative(dy > 0 ? 1 : -1); });
        }
    }
    float stretch = std::clamp(std::abs(ws_pill_.velocity) * 0.035f, 0.0f, 6.0f);
    float pw = pill + stretch;
    float px = std::clamp(r.x + pad + pos * step + pill / 2, r.x + pad + pw / 2, r.right() - pad - pw / 2);
    fill(cr, {px - pw / 2, cy - 6, pw, 12}, 6, p.accent);
}

// ── modules ────────────────────────────────────────────────────────────────
// Each module has a side and a place (config "modules", left to right on
// screen). Workspaces, the focused window, search and the tray are capsules
// of their own; the status ones next to each other share one capsule.

static bool joined_module(const std::string& id) {
    return id == "notifications" || id == "network" || id == "bluetooth" || id == "volume" || id == "battery";
}

static std::string battery_glyph(const BatteryState& b) {
    if (b.charging || b.plugged) return "󰂄";  // the bolt whenever it is on the charger
    return b.percent > 80 ? "󰁹" : b.percent > 50 ? "󰂀" : b.percent > 20 ? "󰁽" : "󰁻";
}

// The performance island: margins, gaps, icon and ring sizes.
constexpr float kPerfPad = 12, kPerfGap = 10, kPerfIcon = 16, kPerfRing = 22;

// Width of a module inside its capsule (0: nothing to show right now).
float App::module_w(const std::string& id) const {
    if (id == "workspaces")
        return niri_.connected() && !niri_.output_workspaces().empty() ? ws_width() : 0;
    if (id == "window") {
        const auto* win = niri_.focused_window();
        if (!win) return 0;
        std::string title = win->title.empty() ? win->app_id : win->title;
        return 12 + 20 + 8 + std::min(240.0f, measure(title, {ui(12, "SemiBold")}).w) + 14;
    }
    if (id == "search") return bar_h();
    if (id == "tray") return bar_h();  // one icon; the apps live in its drop-down
    if (id == "bluetooth") return bt_.state().powered ? 26 : 0;  // only while it is on
    if (id == "battery") {
        const auto& b = power_.battery();
        if (!b.present) return 0;
        return 26 + (config().battery_percent ? measure(std::to_string(b.percent) + "%", {ui(12, "SemiBold")}).w + 4 : 0);
    }
    if (id == "notifications" || id == "network" || id == "volume") return 26;
    if (id == "perf") {
        const auto& ms = config().perf_metrics;
        if (ms.empty()) return 0;
        float w = 0;
        for (auto& m : ms) w += perf_item_w(m);
        return w - kPerfGap + 2 * kPerfPad;  // the same margin both sides
    }
    return 0;
}

// ── the performance island ─────────────────────────────────────────────────
// A few numbers, chosen in the settings. Fixed widths, so the capsule does
// not twitch as they change.

static const char* perf_glyph(const std::string& m) {
    return m == "cpu" ? "󰍛" : m == "ram" ? "󰘚" : m == "temp" ? "󰔏" : m == "net" ? "󰇚" : m == "disk" ? "󰋊" : "󰾴";
}

static std::string short_rate(float bps) {
    char b[16];
    if (bps < 1024 * 1000) snprintf(b, sizeof(b), "%.0fK", bps / 1024);
    else snprintf(b, sizeof(b), "%.1fM", bps / (1024 * 1024));
    return b;
}

// value (0..1, for the ring) and its label
static std::pair<float, std::string> perf_value(const SysStats& s, const std::string& m) {
    auto pct = [](float v) { return std::to_string(static_cast<int>(std::lround(v))) + "%"; };
    if (m == "cpu") return {s.cpu / 100, pct(s.cpu)};
    if (m == "ram") return {s.ram / 100, pct(s.ram)};
    if (m == "disk") return {s.disk / 100, pct(s.disk)};
    if (m == "swap") return {s.swap / 100, pct(s.swap)};
    if (m == "temp") return {s.temp < 0 ? 0 : s.temp / 100, s.temp < 0 ? "–" : std::to_string(static_cast<int>(std::lround(s.temp))) + "°"};
    if (m == "net") return {s.net_hist.empty() ? 0 : s.net_hist.back(), short_rate(s.net_down)};
    return {0, ""};
}

// One item and the gap after it. Numbers: the icon, then the value centred
// in room for its widest form (so nothing shifts as it changes). Rings: the
// ring alone, the icon inside, no number.
float App::perf_item_w(const std::string& m) const {
    if (config().perf_style == "rings") return kPerfRing + kPerfGap;
    static const char* widest[] = {"100%", "100°", "999K"};
    const char* w = m == "temp" ? widest[1] : m == "net" ? widest[2] : widest[0];
    return kPerfIcon + 4 + measure(w, {ui(12, "SemiBold")}).w + kPerfGap;
}

void App::draw_perf(cairo_t* cr, Ui& u, const Rect& r, float origin) {
    const auto& p = T();
    const auto& st = sysmon_.stats();
    bool rings = config().perf_style == "rings";
    float x = r.x + kPerfPad;
    for (auto& m : config().perf_metrics) {
        auto [v, label] = perf_value(st, m);
        float iw = perf_item_w(m) - kPerfGap;
        if (rings) {
            float cx = x + kPerfRing / 2;
            ring(cr, cx, r.cy(), kPerfRing / 2 - 1.5f, 2.6f, std::clamp(v, 0.0f, 1.0f), p.accent, p.text.with_alpha(0.14f));
            glyph(cr, cx, r.cy(), perf_glyph(m), 10, p.text);
        } else {
            glyph(cr, x + kPerfIcon / 2, r.cy(), perf_glyph(m), 13, p.text_dim);
            float tx = x + kPerfIcon + 4, room = iw - kPerfIcon - 4;
            text(cr, tx, r.cy() - 8, label, {ui(12, "SemiBold"), p.text, Align::Center, room});
        }
        x += perf_item_w(m);
    }
    uint64_t mid = ID("mod.perf");
    float ax = origin >= 0 ? origin + r.cx() : -1;
    u.add(mid, r).click([this, ax] {
        if (ax >= 0) anchor_at(ax);
        open(View::Stats);
    });
}

std::vector<App::Cap> App::side_caps(bool left) const {
    std::vector<Cap> caps;
    for (auto& m : config().modules) {
        if (!m.on || (m.side == "left") != left) continue;
        float w = module_w(m.id);
        if (w <= 0) continue;
        if (joined_module(m.id)) {
            if (caps.empty() || !caps.back().joined) caps.push_back({{}, true, 18});
            caps.back().ids.push_back(m.id);
            caps.back().w += w;
        } else {
            caps.push_back({{m.id}, false, w});
        }
    }
    return caps;
}

float App::caps_span(const std::vector<Cap>& caps, float gap) const {
    float s = 0;
    for (auto& c : caps) s += c.w;
    if (!caps.empty()) s += gap * (caps.size() - 1);
    return s;
}

// Grouped layout: capsules either side of the island.
std::vector<Rect> App::side_rects(bool left) const {
    std::vector<Rect> out;
    auto caps = side_caps(left);
    if (caps.empty()) return out;
    Rect ir = island_rect();
    float gap = 8;
    float x = left ? ir.x - 8 - caps_span(caps, gap) : ir.right() + 8;
    for (auto& c : caps) {
        out.push_back(cap_geom(x, c.w));
        x += c.w + gap;
    }
    return out;
}

void App::draw_module(cairo_t* cr, Ui& u, const std::string& id, const Rect& r, float origin) {
    const auto& p = T();
    float ax = origin >= 0 ? origin + r.cx() : -1;  // where it is on screen, for panels dropping from it
    auto here = [this, ax] {
        if (ax >= 0) anchor_at(ax);
    };
    if (id == "workspaces") {
        draw_workspaces(cr, u, r);
        return;
    }
    if (id == "window") {
        const auto* win = niri_.focused_window();
        if (!win) return;
        std::string title = win->title.empty() ? win->app_id : win->title;
        cairo_surface_t* icon = win->app_id.empty() ? nullptr : ImageCache::get().request("icon:" + win->app_id, 40);
        if (icon) image(cr, icon, {r.x + 12, r.cy() - 10, 20, 20}, 4);
        else glyph(cr, r.x + 22, r.cy(), "󰣆", 13, p.text_dim);
        text(cr, r.x + 40, r.cy() - 8, title, {ui(12, "SemiBold"), p.text, Align::Left, r.w - 54});
        u.add(ID("mod.window"), r).click([this] { niri_.action(R"({"Action":{"ToggleOverview":{}}})"); });
        return;
    }
    if (id == "perf") {
        draw_perf(cr, u, r, origin);
        return;
    }
    if (id == "search") {
        uint64_t mid = ID("mod.search");
        float hv = u.hover(mid);
        if (hv > 0.01f) fill(cr, r.inset(4), (r.h - 8) / 2, p.text.with_alpha(0.09f * hv));
        glyph(cr, r.cx(), r.cy(), "󰍉", 15, p.text);
        u.add(mid, r).click([this, here] {
            here();
            open(View::Launcher);
        });
        return;
    }
    if (id == "tray") {
        uint64_t mid = ID("mod.tray");
        float hv = u.hover(mid);
        bool open_now = !nav_.empty() && nav_.back() == View::Tray;
        if (hv > 0.01f || open_now) fill(cr, r.inset(4), (r.h - 8) / 2, p.text.with_alpha(open_now ? 0.12f : 0.09f * hv));
        glyph(cr, r.cx(), r.cy(), "󰀻", 15, p.text);
        // an app asking for attention: the accent, on the icon itself (no badge)
        bool attention = false;
        for (auto& it : tray_.items()) attention |= it.status == "NeedsAttention";
        if (attention) glyph(cr, r.cx(), r.cy(), "󰀻", 15, p.accent);
        u.add(mid, r).click([this, here, open_now] {
            if (open_now) {
                close_all();
                return;
            }
            here();
            tray_menu_key_.clear();
            open(View::Tray);
        });
        return;
    }
    // the status ones
    Rect hr{r.x, r.y + 4, r.w, r.h - 8};
    uint64_t mid = ID("mod", static_cast<int64_t>(std::hash<std::string>{}(id) & 0xffffff));
    float hv = u.hover(mid);
    if (hv > 0.01f) fill(cr, hr, hr.h / 2, p.text.with_alpha(0.09f * hv));
    Region& reg = u.add(mid, hr);
    reg.right([this, here] {
        here();
        open(View::Control);
    });
    float gx = r.x + 13;
    if (id == "notifications") {
        bool dnd = notifs_.dnd();
        glyph(cr, gx, r.cy(), dnd ? "󰂛" : "󰂚", 14, dnd ? p.warn : p.text);
        if (!notifs_.history().empty() && !dnd) fill(cr, {gx + 3, r.cy() - 8, 6, 6}, 3, p.accent);
        reg.click([this, here] {
            here();
            open(View::Inbox);
        });
        reg.middle([this] { notifs_.cycle_dnd(); });
    } else if (id == "network") {
        const auto& ns = net_.state();
        std::string g = !ns.wifi_enabled ? "󰤭"
                      : ns.ssid.empty()  ? (ns.ethernet ? "󰈀" : "󰤯")
                      : ns.signal > 66   ? "󰤨"
                      : ns.signal > 33   ? "󰤥"
                                         : "󰤟";
        glyph(cr, gx, r.cy(), g.c_str(), 14, ns.wifi_enabled || ns.ethernet ? p.text : p.text_faint);
        reg.click([this, here] {
            here();
            open(View::Wifi);
        });
    } else if (id == "bluetooth") {
        const auto& bs = bt_.state();
        bool any = std::any_of(bs.devices.begin(), bs.devices.end(), [](const BtDevice& d) { return d.connected; });
        glyph(cr, gx, r.cy(), any ? "󰂱" : "󰂯", 14, p.text);
        reg.click([this, here] {
            here();
            open(View::Bluetooth);
        });
    } else if (id == "volume") {
        const auto& s = audio_.state().sink;
        std::string g = s.muted || s.volume < 0.005f ? "󰝟" : s.volume < 0.34f ? "󰕿" : s.volume < 0.67f ? "󰖀" : "󰕾";
        glyph(cr, gx, r.cy(), g.c_str(), 14, s.muted ? p.text_faint : p.text);
        reg.click([this, here] {
               here();
               open(View::Sound);
           })
            .middle([this] { audio_.toggle_mute(); })
            .scroll([this](float dy) { audio_.change_volume(-dy * 0.05f); });
    } else if (id == "battery") {
        const auto& b = power_.battery();
        Color c = p.text;  // follows the theme, charging or not
        glyph(cr, gx, r.cy(), battery_glyph(b).c_str(), 14, c);
        if (config().battery_percent) text(cr, r.x + 24, r.cy() - 8, std::to_string(b.percent) + "%", {ui(12, "SemiBold"), c});
        reg.click([this, here] {
            here();
            OsdInfo o;  // pinned: it stays a while and an outside click closes it
            o.kind = OsdInfo::Battery;
            o.label = power_.battery().charging ? "Cargando" : "Batería";
            show_osd(o, 8.0);
            osd_pinned_ = true;
            wl_.set_shield(true);
        });
    }
}

// One capsule: its background (unless inside the island) and its modules.
void App::draw_cap(cairo_t* cr, Ui& u, const Rect& r, const Cap& c, float origin, bool background) {
    if (background) capsule(cr, r);
    if (!c.joined) {
        draw_module(cr, u, c.ids.front(), r, origin);
        return;
    }
    float x = r.x + 9;
    for (auto& id : c.ids) {
        float w = module_w(id);
        draw_module(cr, u, id, {x, r.y, w, r.h}, origin);
        x += w;
    }
}

// ── island: everything in one capsule ─────────────────────────────────────

float App::one_island_side() const {
    if (bar_layout() != "island") return 0;
    // both sides as wide as the wider one: the clock stays in the middle
    return std::max(caps_span(side_caps(true), 4), caps_span(side_caps(false), 4));
}

void App::draw_island_sides(cairo_t* cr, float w, float h) {
    float side = one_island_side();
    for (bool left : {true, false}) {
        auto caps = side_caps(left);
        float span = caps_span(caps, 4);
        float x = left ? (side - span) : w - side;
        for (auto& c : caps) {
            draw_cap(cr, ui_, {x, 0, c.w, h}, c, -1, false);
            x += c.w + 4;
        }
    }
}

// ── grouped: capsules beside the island ───────────────────────────────────

static Rect unite_all(const std::vector<Rect>& rs) {
    Rect u{};
    for (auto& r : rs) u = u.empty() ? r : u.unite(r);
    return u;
}

Rect App::bubble_rect() const {
    if (ws_alpha_.value < 0.02f) return {};
    return unite_all(side_rects(true));
}

Rect App::status_rect() const {
    if (st_alpha_.value < 0.02f) return {};
    return unite_all(side_rects(false));
}

void App::draw_side(cairo_t* cr, bool left) {
    float a = clamp01(left ? ws_alpha_.value : st_alpha_.value);
    if (a < 0.02f) return;
    auto caps = side_caps(left);
    auto rects = side_rects(left);
    if (rects.empty()) return;
    Rect all = unite_all(rects);
    cairo_save(cr);
    // slides out of the island as it appears
    cairo_translate(cr, (left ? 1 : -1) * (1.0f - a) * 18.0f, 0);
    bool fading = a < 0.999f;
    if (fading) {
        cairo_rectangle(cr, all.x - 24, all.y - 2, all.w + 48, all.h + 30);
        cairo_clip(cr);
        cairo_push_group(cr);
    }
    for (size_t i = 0; i < caps.size() && i < rects.size(); ++i) draw_cap(cr, ui_, rects[i], caps[i], -1, true);
    if (fading) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, a);
    }
    cairo_restore(cr);
}

// ── spread: the corner strips ─────────────────────────────────────────────

void App::paint_edge(cairo_t* cr, Ui& u, float w, float h, bool left, std::vector<Rect>& input) {
    (void)h;
    auto caps = side_caps(left);
    float gap = 12;
    float inset = EdgeBar::edge_inset();
    float x = left ? inset : w - inset - caps_span(caps, gap);
    float origin = left ? 0 : surface_w() - EdgeBar::kWidth;  // the strip's left edge on screen
    for (auto& c : caps) {
        Rect r = cap_geom(x, c.w);
        draw_cap(cr, u, r, c, origin, true);
        input.push_back(r);
        x += c.w + gap;
    }
}

// niri's own outer gap ("gaps N" in its layout), which it adds below any
// reserved zone. Read from its config so the island's gap means what it says.
static int niri_gaps() {
    std::vector<std::string> files = {home_dir() + "/.config/niri/config.kdl"};
    for (const char* f : {"layout.kdl", "cfg/layout.kdl"}) files.push_back(home_dir() + "/.config/niri/" + f);
    files.push_back(home_dir() + "/.config/niri/cfg/layout.kdl");
    for (auto& f : files) {
        std::string text;
        if (!read_file(f, text)) continue;
        for (auto& line : split(text, '\n')) {
            std::string t = trim(line);
            if (t.rfind("gaps ", 0) != 0) continue;
            int v = std::atoi(t.c_str() + 5);
            if (v >= 0) return v;
        }
    }
    return 16;  // niri's default
}

// The island's own niri rules (xray for the blur behind it), in a file niri
// includes. Written only when it changes: each write makes niri reload.
static void write_niri_rules() {
    std::string path = home_dir() + "/.config/niri/cfg/niri-island.kdl";
    std::string body = "// Generado por niri-island. Se reescribe solo.\n"
        // The wallpaper, still or animated (mpvpaper), is one backdrop behind
        // every workspace in the overview, not a copy inside each.
        "layer-rule {\n"
        "    match namespace=\"^niri-island-wallpaper$\"\n"
        "    match namespace=\"^mpvpaper$\"\n"
        "    place-within-backdrop true\n"
        "}\n";
    if (config().compositor_blur) {
        // Only the island and its strips: a rule on every niri-island-* surface
        // blurred the full-screen settings window too, until it was destroyed.
        body +=
            "layer-rule {\n"
            "    match namespace=\"^niri-island$\"\n"
            "    match namespace=\"^niri-island-bar-\"\n"
            "    match namespace=\"^niri-island-capbar$\"\n"
            "    background-effect {\n"
            "        xray " + std::string(config().blur_xray ? "true" : "false") + "\n"
            "    }\n"
            "}\n";
    }
    std::string old;
    if (read_file(path, old) && old == body) return;
    if (!file_exists(home_dir() + "/.config/niri/cfg")) return;
    write_file_atomic(path, body);
}

// Applies the bar layout and island style (start-up and config reloads).
void App::apply_layout() {
    bool spread = bar_layout() == "spread";
    for (EdgeBar* b : {&bar_left_, &bar_right_}) {
        if (spread) {
            b->open();
            b->restyle();  // margins follow the style (a reused surface kept the old ones)
        } else if (b->exists()) {
            b->close();
        }
    }
    layout_key_ = config().island_style;
    write_niri_rules();
    if (!island_) return;
    island_->set_margin(attached() ? 0 : config().margin_top);
    // Windows start at margin + zone + niri's gap (niri counts the margin for
    // a surface anchored to three edges); the island ends at margin + its height.
    int zone = std::max(1, config().bar_height + config().window_gap - niri_gaps());
    island_->set_exclusive_zone(config().reserve_space ? zone : 0);
    request();
}

}  // namespace island
