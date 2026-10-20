#pragma once

#include <cairo.h>
#include <functional>
#include <string>

#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/ui.hpp"

namespace island::w {

// Every widget registers its own region, so drawing and hit testing never
// disagree. They read Theme and animate through Ui's per-id springs.

struct Ctx {
    cairo_t* cr;
    Ui& ui;
};

// Hover/press-reactive background for any clickable rect; returns the region.
Region& hit(Ctx& c, uint64_t id, const Rect& r);

// Round icon button (glyph). `active` paints it with the accent.
Region& icon_button(Ctx& c, uint64_t id, const Rect& r, const char* glyph, float glyph_size = 15,
                    bool active = false, Color tint = {0, 0, 0, 0});

// Capsule with optional glyph + label.
enum class Style { Plain, Tonal, Accent, Danger, Ghost };
Region& pill(Ctx& c, uint64_t id, const Rect& r, const std::string& label, const char* glyph = nullptr,
             Style style = Style::Tonal, float font_px = 12.5f);

// Panel header: back button, title, optional subtitle. Returns the back region.
Region& header(Ctx& c, uint64_t id, float w, const std::string& title, const std::string& subtitle = "",
               bool back = true);

void card(Ctx& c, const Rect& r, float radius = 16, float hover = 0);

void toggle(Ctx& c, uint64_t id, const Rect& r, bool on, std::function<void()> on_toggle);

// Horizontal slider; calls on_change(0..1) while dragging. Draws the glyph on
// the left and the percentage on the right inside the track.
void slider(Ctx& c, uint64_t id, const Rect& r, float value, const char* glyph, std::function<void(float)> on_change,
            std::function<void()> on_glyph = {}, const std::string& right_label = "", bool muted = false);

// A list row card with icon area, title, subtitle; returns the region.
Region& row(Ctx& c, uint64_t id, const Rect& r, const char* glyph, const std::string& title,
            const std::string& subtitle, bool active = false, Color glyph_color = {0, 0, 0, 0});

// Vertical scrollbar indicator.
void scrollbar(Ctx& c, const Rect& track, float offset, float content, float view);

// Tactile scale for pressed controls.
float press_scale(Ui& ui, uint64_t id, float amount = 0.06f);

}  // namespace island::w
