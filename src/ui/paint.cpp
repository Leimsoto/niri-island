#include "ui/paint.hpp"

#include <pango/pangocairo.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "core/config.hpp"

namespace island::paint {

static std::unordered_map<std::string, PangoFontDescription*>& font_cache() {
    static std::unordered_map<std::string, PangoFontDescription*> c;
    return c;
}


static PangoFontDescription* font_desc(const std::string& s) {
    auto& c = font_cache();
    auto it = c.find(s);
    if (it != c.end()) return it->second;
    PangoFontDescription* d = pango_font_description_from_string(s.c_str());
    c[s] = d;
    return d;
}

std::string ui(float size, const char* weight) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s %.1fpx", config().font.c_str(), weight, size);
    return buf;
}

std::string hand(float size, const char* weight) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s %.1fpx", config().note_font.c_str(), weight, size);
    return buf;
}

std::string mono(float size, const char* weight) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s %s %.1fpx", config().mono_font.c_str(), weight, size);
    return buf;
}

static PangoContext* shared_context() {
    static PangoContext* ctx = nullptr;
    if (!ctx) {
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        ctx = pango_font_map_create_context(fm);
        cairo_font_options_t* opts = cairo_font_options_create();
        cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(opts, CAIRO_HINT_STYLE_SLIGHT);
        cairo_font_options_set_hint_metrics(opts, CAIRO_HINT_METRICS_OFF);
        pango_cairo_context_set_font_options(ctx, opts);
        cairo_font_options_destroy(opts);
    }
    return ctx;
}

// Laid-out text, reused while the same string is drawn with the same font and
// geometry: shaping the clock alone was a third of a frame.
namespace {
struct LayoutEntry {
    PangoLayout* layout;
    uint64_t used;
};
std::unordered_map<std::string, LayoutEntry>& layout_cache() {
    static std::unordered_map<std::string, LayoutEntry> c;
    return c;
}
uint64_t g_layout_tick = 0;
}  // namespace

static void configure(PangoLayout* l, std::string_view s, const Text& t);

static PangoLayout* prepare(cairo_t* cr, std::string_view s, const Text& t) {
    std::string key;
    key.reserve(s.size() + t.font.size() + 32);
    key += t.font;
    key += '|';
    key += std::to_string(static_cast<int>(t.width * 4));
    key += t.wrap ? 'w' : '-';
    key += t.markup ? 'm' : '-';
    key += static_cast<char>('0' + static_cast<int>(t.align));
    key += std::to_string(t.lines);
    key += '|';
    key.append(s.data(), s.size());
    auto& cache = layout_cache();
    ++g_layout_tick;
    auto it = cache.find(key);
    PangoLayout* l;
    if (it != cache.end()) {
        it->second.used = g_layout_tick;
        l = it->second.layout;
    } else {
        if (cache.size() >= 400) {
            // Drop the least recently used half.
            std::vector<std::pair<uint64_t, std::string>> order;
            order.reserve(cache.size());
            for (auto& [k, e] : cache) order.emplace_back(e.used, k);
            std::sort(order.begin(), order.end());
            for (size_t i = 0; i < order.size() / 2; ++i) {
                auto f = cache.find(order[i].second);
                g_object_unref(f->second.layout);
                cache.erase(f);
            }
        }
        l = pango_layout_new(shared_context());
        configure(l, s, t);
        cache.emplace(std::move(key), LayoutEntry{l, g_layout_tick});
    }
    if (cr) pango_cairo_update_layout(cr, l);
    return l;
}

static void configure(PangoLayout* l, std::string_view s, const Text& t) {
    pango_layout_set_font_description(l, font_desc(t.font));
    pango_layout_set_attributes(l, nullptr);
    bool set = false;
    if (t.markup) {
        std::string m(s);
        if (pango_parse_markup(m.c_str(), -1, 0, nullptr, nullptr, nullptr, nullptr)) {
            pango_layout_set_markup(l, m.c_str(), -1);
            set = true;
        }
    }
    if (!set) pango_layout_set_text(l, s.data(), static_cast<int>(s.size()));
    if (t.width > 0) {
        pango_layout_set_width(l, static_cast<int>(t.width * PANGO_SCALE));
        if (t.wrap) {
            pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
            pango_layout_set_height(l, t.lines > 0 ? -t.lines : -1);
            pango_layout_set_ellipsize(l, t.lines > 0 ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE);
        } else {
            pango_layout_set_height(l, -1);
            pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
        }
    } else {
        pango_layout_set_width(l, -1);
        pango_layout_set_height(l, -1);
        pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_NONE);
    }
    pango_layout_set_alignment(l, t.align == Align::Center  ? PANGO_ALIGN_CENTER
                                  : t.align == Align::Right ? PANGO_ALIGN_RIGHT
                                                            : PANGO_ALIGN_LEFT);
}

void set(cairo_t* cr, const Color& c) { cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a); }

void rrect(cairo_t* cr, const Rect& r, float radius) {
    float rad = std::max(0.0f, std::min({radius, r.w / 2.0f, r.h / 2.0f}));
    cairo_new_sub_path(cr);
    if (rad <= 0.01f) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        return;
    }
    cairo_arc(cr, r.x + r.w - rad, r.y + rad, rad, -M_PI / 2, 0);
    cairo_arc(cr, r.x + r.w - rad, r.y + r.h - rad, rad, 0, M_PI / 2);
    cairo_arc(cr, r.x + rad, r.y + r.h - rad, rad, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + rad, r.y + rad, rad, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void fill(cairo_t* cr, const Rect& r, float radius, const Color& c) {
    if (c.a <= 0.001f || r.empty()) return;
    rrect(cr, r, radius);
    set(cr, c);
    cairo_fill(cr);
}

void stroke(cairo_t* cr, const Rect& r, float radius, const Color& c, float width) {
    if (c.a <= 0.001f || r.empty()) return;
    Rect h{r.x + width / 2, r.y + width / 2, r.w - width, r.h - width};
    rrect(cr, h, radius - width / 2);
    set(cr, c);
    cairo_set_line_width(cr, width);
    cairo_stroke(cr);
}

void shadow(cairo_t* cr, const Rect& r, float radius, float alpha, float spread) {
    if (alpha <= 0.001f) return;
    const int steps = 7;
    for (int i = steps; i >= 1; --i) {
        float k = static_cast<float>(i) / steps;
        float grow = spread * k;
        Rect s{r.x - grow * 0.7f, r.y - grow * 0.45f, r.w + grow * 1.4f, r.h + grow * 1.15f};
        rrect(cr, s, radius + grow);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha * (1.0f - k) * (1.0f - k) * 0.55f);
        cairo_fill(cr);
    }
}

void shadow_outside(cairo_t* cr, const Rect& r, float radius, float alpha, float spread) {
    cairo_save(cr);
    rrect(cr, r, radius);
    Rect o = r.inset(-spread * 2 - 8);
    cairo_rectangle(cr, o.x, o.y, o.w, o.h);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_clip(cr);
    shadow(cr, r, radius, alpha, spread);
    cairo_restore(cr);
}

void sheen(cairo_t* cr, const Rect& r, float radius, float alpha) {
    if (alpha <= 0.001f) return;
    cairo_save(cr);
    rrect(cr, r, radius);
    cairo_clip(cr);
    cairo_pattern_t* g = cairo_pattern_create_linear(r.x, r.y, r.x, r.y + std::min(48.0f, r.h));
    cairo_pattern_add_color_stop_rgba(g, 0.0, 1, 1, 1, alpha);
    cairo_pattern_add_color_stop_rgba(g, 1.0, 1, 1, 1, 0);
    cairo_set_source(cr, g);
    cairo_paint(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
}

Size text(cairo_t* cr, float x, float y, std::string_view s, const Text& t) {
    if (s.empty()) return {};
    PangoLayout* l = prepare(cr, s, t);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(l, &ink, &log);
    float dx = 0;
    if (t.width <= 0) {
        if (t.align == Align::Center) dx = -log.width / 2.0f;
        else if (t.align == Align::Right) dx = -static_cast<float>(log.width);
    }
    cairo_move_to(cr, x + dx, y);
    set(cr, t.color);
    pango_cairo_show_layout(cr, l);
    return {static_cast<float>(log.width), static_cast<float>(log.height)};
}

Size measure(std::string_view s, const Text& t) {
    if (s.empty()) return {};
    PangoLayout* l = prepare(nullptr, s, t);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(l, &ink, &log);
    return {static_cast<float>(log.width), static_cast<float>(log.height)};
}

Size text_in(cairo_t* cr, const Rect& area, std::string_view s, Text t) {
    if (t.width <= 0) t.width = area.w;
    Size sz = measure(s, t);
    return text(cr, area.x, area.y + (area.h - sz.h) / 2.0f, s, t);
}

float caret_x(std::string_view s, const Text& t, size_t byte_index) {
    if (s.empty()) return 0;
    Text plain = t;
    plain.width = -1;
    plain.markup = false;
    PangoLayout* l = prepare(nullptr, s, plain);
    PangoRectangle pos;
    pango_layout_index_to_pos(l, static_cast<int>(std::min(byte_index, s.size())), &pos);
    return pos.x / static_cast<float>(PANGO_SCALE);
}

size_t index_at(std::string_view s, const Text& t, float x) {
    if (s.empty()) return 0;
    Text plain = t;
    plain.width = -1;
    plain.markup = false;
    PangoLayout* l = prepare(nullptr, s, plain);
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(l, static_cast<int>(x * PANGO_SCALE), 0, &index, &trailing);
    size_t i = static_cast<size_t>(index);
    std::string str(s);
    while (trailing-- > 0) i = utf8_next(str, i);
    return std::min(i, s.size());
}

Rect caret_rect(std::string_view s, const Text& t, size_t byte_index) {
    Text plain = t;
    plain.markup = false;
    std::string str = s.empty() ? std::string(" ") : std::string(s);
    PangoLayout* l = prepare(nullptr, str, plain);
    PangoRectangle pos;
    pango_layout_index_to_pos(l, static_cast<int>(std::min(byte_index, s.size())), &pos);
    return {pos.x / float(PANGO_SCALE), pos.y / float(PANGO_SCALE), 1.6f, pos.height / float(PANGO_SCALE)};
}

size_t index_at_xy(std::string_view s, const Text& t, float x, float y) {
    if (s.empty()) return 0;
    Text plain = t;
    plain.markup = false;
    PangoLayout* l = prepare(nullptr, s, plain);
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(l, static_cast<int>(x * PANGO_SCALE), static_cast<int>(y * PANGO_SCALE), &index, &trailing);
    size_t i = static_cast<size_t>(index);
    std::string str(s);
    while (trailing-- > 0) i = utf8_next(str, i);
    return std::min(i, s.size());
}

void glyph(cairo_t* cr, float cx, float cy, std::string_view g, float size, const Color& c) {
    if (g.empty() || c.a <= 0.001f) return;
    Text t;
    t.font = mono(size);
    PangoLayout* l = prepare(cr, g, t);
    PangoRectangle ink, log;
    pango_layout_get_pixel_extents(l, &ink, &log);
    cairo_move_to(cr, cx - ink.width / 2.0f - ink.x, cy - ink.height / 2.0f - ink.y);
    set(cr, c);
    pango_cairo_show_layout(cr, l);
}

void image(cairo_t* cr, cairo_surface_t* img, const Rect& r, float radius, float alpha) {
    if (!img || r.empty()) return;
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return;
    cairo_save(cr);
    rrect(cr, r, radius);
    cairo_clip(cr);
    double s = std::max(r.w / iw, r.h / ih);  // cover
    double dw = iw * s, dh = ih * s;
    cairo_translate(cr, r.x + (r.w - dw) / 2.0, r.y + (r.h - dh) / 2.0);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    if (alpha >= 0.999f) cairo_paint(cr);
    else cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

void ring(cairo_t* cr, float cx, float cy, float radius, float width, float frac, const Color& fg, const Color& track) {
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, radius, 0, 2 * M_PI);
    set(cr, track);
    cairo_stroke(cr);
    frac = clamp01(frac);
    if (frac > 0.001f) {
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, radius, -M_PI / 2, -M_PI / 2 + 2 * M_PI * frac);
        set(cr, fg);
        cairo_stroke(cr);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
}

void bar(cairo_t* cr, const Rect& r, float frac, const Color& fg, const Color& track) {
    fill(cr, r, r.h / 2, track);
    frac = clamp01(frac);
    if (frac > 0.001f) fill(cr, {r.x, r.y, std::max(r.h, r.w * frac), r.h}, r.h / 2, fg);
}

void sparkline(cairo_t* cr, const Rect& r, const std::vector<float>& values, const Color& c, bool filled) {
    if (values.size() < 2) return;
    size_t n = values.size();
    auto px = [&](size_t i) { return r.x + r.w * static_cast<float>(i) / static_cast<float>(n - 1); };
    auto py = [&](size_t i) { return r.y + r.h * (1.0f - clamp01(values[i])); };
    cairo_new_path(cr);
    cairo_move_to(cr, px(0), py(0));
    for (size_t i = 1; i < n; ++i) {
        float x0 = px(i - 1), y0 = py(i - 1), x1 = px(i), y1 = py(i);
        float mx = (x0 + x1) / 2;
        cairo_curve_to(cr, mx, y0, mx, y1, x1, y1);
    }
    if (filled) {
        cairo_path_t* line = cairo_copy_path(cr);
        cairo_line_to(cr, r.right(), r.bottom());
        cairo_line_to(cr, r.x, r.bottom());
        cairo_close_path(cr);
        cairo_pattern_t* g = cairo_pattern_create_linear(0, r.y, 0, r.bottom());
        cairo_pattern_add_color_stop_rgba(g, 0, c.r, c.g, c.b, c.a * 0.35);
        cairo_pattern_add_color_stop_rgba(g, 1, c.r, c.g, c.b, 0);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        cairo_append_path(cr, line);
        cairo_path_destroy(line);
    }
    set(cr, c);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(cr);
}

void divider(cairo_t* cr, float x1, float y1, float x2, float y2, const Color& c) {
    set(cr, c);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
}

void scale_around(cairo_t* cr, float cx, float cy, float s) {
    if (std::abs(s - 1.0f) < 0.0005f) return;
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, s, s);
    cairo_translate(cr, -cx, -cy);
}

void reset_fonts() {
    for (auto& [k, e] : layout_cache()) g_object_unref(e.layout);
    layout_cache().clear();
    for (auto& [k, v] : font_cache()) pango_font_description_free(v);
    font_cache().clear();
}

}  // namespace island::paint
