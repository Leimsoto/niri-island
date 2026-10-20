#include "ui/widgets.hpp"

namespace island::w {

using namespace paint;

float press_scale(Ui& ui, uint64_t id, float amount) { return 1.0f - ui.press(id) * amount; }

Region& hit(Ctx& c, uint64_t id, const Rect& r) { return c.ui.add(id, r); }

Region& icon_button(Ctx& c, uint64_t id, const Rect& r, const char* glyph, float glyph_size, bool active, Color tint) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    float s = press_scale(c.ui, id, 0.12f);
    cairo_save(c.cr);
    scale_around(c.cr, r.cx(), r.cy(), s);
    float rad = std::min(r.w, r.h) / 2;
    if (active) {
        fill(c.cr, r, rad, mix(p.accent_dim, p.accent, 0.15f * hv));
    } else if (hv > 0.001f) {
        fill(c.cr, r, rad, p.text.with_alpha(0.08f * hv));
    }
    Color g = tint.a > 0 ? tint : (active ? p.accent : mix(p.text_dim, p.text, hv));
    paint::glyph(c.cr, r.cx(), r.cy(), glyph, glyph_size, g);
    cairo_restore(c.cr);
    return c.ui.add(id, r);
}

Region& pill(Ctx& c, uint64_t id, const Rect& r, const std::string& label, const char* g, Style style, float font_px) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    float s = press_scale(c.ui, id, 0.05f);
    cairo_save(c.cr);
    scale_around(c.cr, r.cx(), r.cy(), s);
    Color bg, fg;
    switch (style) {
        case Style::Accent:
            bg = mix(p.accent, Color{1, 1, 1, 1}, 0.12f * hv);
            fg = p.on_accent;
            break;
        case Style::Danger:
            bg = mix(p.bad.with_alpha(0.20f), p.bad.with_alpha(0.32f), hv);
            fg = mix(p.bad, Color{1, 1, 1, 1}, 0.3f);
            break;
        case Style::Plain:
            bg = mix(p.surface, p.surface_hi, hv);
            fg = p.text;
            break;
        case Style::Ghost:
            bg = p.text.with_alpha(0.07f * hv);
            fg = mix(p.text_dim, p.text, hv);
            break;
        default:
            bg = mix(p.accent_dim, mix(p.accent_dim, p.accent, 0.25f), hv);
            fg = mix(p.accent, Color{1, 1, 1, 1}, 0.25f);
    }
    fill(c.cr, r, r.h / 2, bg);
    Text t{ui(font_px, "SemiBold"), fg};
    Size ts = label.empty() ? Size{} : measure(label, t);
    float gw = g ? font_px + 2 : 0;
    float gap = (g && !label.empty()) ? 6 : 0;
    float total = gw + gap + ts.w;
    float x = r.cx() - total / 2;
    if (g) paint::glyph(c.cr, x + gw / 2, r.cy(), g, font_px + 1.5f, fg);
    if (!label.empty()) text(c.cr, x + gw + gap, r.cy() - ts.h / 2, label, t);
    cairo_restore(c.cr);
    return c.ui.add(id, r);
}

Region& header(Ctx& c, uint64_t id, float w, const std::string& title, const std::string& subtitle, bool back) {
    const auto& p = T();
    static Region dummy;
    float x = 18;
    Region* reg = &dummy;
    if (back) {
        reg = &icon_button(c, id, {12, 12, 34, 34}, "󰁍", 16);
        x = 54;
    }
    if (subtitle.empty()) {
        text(c.cr, x, 19, title, {ui(16, "Bold"), p.text, Align::Left, w - x - 120});
    } else {
        text(c.cr, x, 11, title, {ui(15.5f, "Bold"), p.text, Align::Left, w - x - 120});
        text(c.cr, x, 31, subtitle, {ui(11.5f), p.text_dim, Align::Left, w - x - 120});
    }
    return *reg;
}

void card(Ctx& c, const Rect& r, float radius, float hover) {
    const auto& p = T();
    fill(c.cr, r, radius, mix(p.surface, p.surface_hi, hover));
    stroke(c.cr, r, radius, p.border, 1.0f);
}

void toggle(Ctx& c, uint64_t id, const Rect& r, bool on, std::function<void()> on_toggle) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    Rect track{r.x, r.cy() - 12, 42, 24};
    fill(c.cr, track, 12, on ? p.accent : mix(p.surface_hi, p.text.with_alpha(0.2f), 0.4f + 0.3f * hv));
    float knob = 18;
    float kx = on ? track.right() - knob - 3 : track.x + 3;
    float press = c.ui.press(id);
    float kw = knob + press * 4;
    if (on) kx -= press * 4;
    fill(c.cr, {kx, track.cy() - knob / 2, kw, knob}, knob / 2, on ? p.on_accent : p.text);
    c.ui.add(id, track.inset(-4)).click(std::move(on_toggle));
}

void slider(Ctx& c, uint64_t id, const Rect& r, float value, const char* g, std::function<void(float)> on_change,
            std::function<void()> on_glyph, const std::string& right_label, bool muted) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    float pr = c.ui.press(id);
    value = clamp01(value);
    // Pressed tracks swell a little, like impasto's LevelRow.
    Rect t = r.inset(-pr * 1.5f, -pr * 1.5f);
    float rad = t.h / 2;
    fill(c.cr, t, rad, mix(p.surface_hi, p.text.with_alpha(0.16f), hv * 0.4f));
    float fw = std::max(t.h, t.w * value);
    Color fillc = muted ? p.text_faint : mix(p.accent_dim, p.accent, 0.55f + 0.25f * hv);
    if (value > 0.001f) fill(c.cr, {t.x, t.y, fw, t.h}, rad, fillc);
    std::string pct = right_label.empty() ? std::to_string(static_cast<int>(std::lround(value * 100))) + "%" : right_label;
    text(c.cr, t.right() - 14, t.cy() - 8, pct, {ui(12, "SemiBold"), p.text, Align::Right});
    Rect gl{t.x + 4, t.y, t.h, t.h};
    if (g) paint::glyph(c.cr, gl.cx() + 4, gl.cy(), g, 15, value > 0.12f && !muted ? p.on_accent : p.text);
    c.ui.add(id, r).drag([on_change](const DragEvent& e) { on_change(clamp01(e.fx)); })
        .cursor_shape(CursorShape::Pointer)
        .scroll([on_change, value](float dy) { on_change(clamp01(value - dy * 0.05f)); });
    if (on_glyph) c.ui.add(id + 1, {gl.x, gl.y, gl.w + 8, gl.h}).click(std::move(on_glyph));
}

Region& row(Ctx& c, uint64_t id, const Rect& r, const char* g, const std::string& title, const std::string& subtitle,
            bool active, Color glyph_color) {
    const auto& p = T();
    float hv = c.ui.hover(id);
    float s = press_scale(c.ui, id, 0.025f);
    cairo_save(c.cr);
    scale_around(c.cr, r.cx(), r.cy(), s);
    fill(c.cr, r, 14, active ? mix(p.accent_dim, p.surface_hi, 0.15f * hv) : mix(p.surface, p.surface_hi, hv));
    if (active) stroke(c.cr, r, 14, p.accent.with_alpha(0.35f), 1.0f);
    Rect badge{r.x + 9, r.cy() - 16, 32, 32};
    fill(c.cr, badge, 16, active ? p.accent : p.surface_hi);
    Color gc = glyph_color.a > 0 ? glyph_color : (active ? p.on_accent : p.text_dim);
    paint::glyph(c.cr, badge.cx(), badge.cy(), g, 15, gc);
    float tx = badge.right() + 11;
    float tw = r.right() - tx - 50;
    if (subtitle.empty()) {
        text_in(c.cr, {tx, r.y, tw, r.h}, title, {ui(13.5f, "SemiBold"), p.text});
    } else {
        text(c.cr, tx, r.cy() - 17, title, {ui(13.5f, "SemiBold"), p.text, Align::Left, tw});
        text(c.cr, tx, r.cy() + 1, subtitle, {ui(11.5f), active ? p.accent : p.text_dim, Align::Left, tw});
    }
    cairo_restore(c.cr);
    return c.ui.add(id, r);
}

void scrollbar(Ctx& c, const Rect& track, float offset, float content, float view) {
    if (content <= view + 16) return;  // trailing gaps don't count as overflow
    const auto& p = T();
    float th = std::max(24.0f, track.h * view / content);
    float maxo = content - view;
    float ty = track.y + (track.h - th) * clamp01(offset / maxo);
    fill(c.cr, {track.x, ty, track.w, th}, track.w / 2, p.text.with_alpha(0.25f));
}

}  // namespace island::w
