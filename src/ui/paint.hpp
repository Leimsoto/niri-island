#pragma once

#include <cairo.h>
#include <string>
#include <string_view>
#include <vector>

#include "core/util.hpp"

namespace island::paint {

enum class Align { Left, Center, Right };

struct Size {
    float w = 0, h = 0;
};

struct Text {
    std::string font;
    Color color{1, 1, 1, 1};
    Align align = Align::Left;
    float width = -1;     // wrap/ellipsize width; -1 = natural
    int lines = 1;        // max lines (wrap); 0 = unlimited
    bool markup = false;
    bool wrap = false;
};

// Font strings built from the configured families.
std::string ui(float size, const char* weight = "");
std::string mono(float size, const char* weight = "");
std::string hand(float size, const char* weight = "");  // handwriting (notes)

void rrect(cairo_t* cr, const Rect& r, float radius);
void fill(cairo_t* cr, const Rect& r, float radius, const Color& c);
void stroke(cairo_t* cr, const Rect& r, float radius, const Color& c, float width = 1.0f);
void set(cairo_t* cr, const Color& c);
// Soft drop shadow under a rounded rect.
void shadow(cairo_t* cr, const Rect& r, float radius, float alpha, float spread = 14.0f);
// The same, kept outside the rounded rect: for translucent bodies.
void shadow_outside(cairo_t* cr, const Rect& r, float radius, float alpha, float spread = 14.0f);
// Diagonal sheen across a card: the glassy edge impasto uses.
void sheen(cairo_t* cr, const Rect& r, float radius, float alpha);

Size text(cairo_t* cr, float x, float y, std::string_view s, const Text& t);
Size measure(std::string_view s, const Text& t);
// Vertically centred in `area` (left/centre/right by t.align within area.w).
Size text_in(cairo_t* cr, const Rect& area, std::string_view s, Text t);
// X offset of a byte index inside a single-line text, for carets.
float caret_x(std::string_view s, const Text& t, size_t byte_index);
// Byte index nearest to x inside a single-line text.
size_t index_at(std::string_view s, const Text& t, float x);
// Caret rectangle for a byte index in wrapped text (layout coordinates).
Rect caret_rect(std::string_view s, const Text& t, size_t byte_index);
// Byte index under a point of wrapped text.
size_t index_at_xy(std::string_view s, const Text& t, float x, float y);

// An icon glyph centred on its ink.
void glyph(cairo_t* cr, float cx, float cy, std::string_view g, float size, const Color& c);

void image(cairo_t* cr, cairo_surface_t* img, const Rect& r, float radius, float alpha = 1.0f);
void ring(cairo_t* cr, float cx, float cy, float radius, float width, float frac, const Color& fg, const Color& track);
void bar(cairo_t* cr, const Rect& r, float frac, const Color& fg, const Color& track);
void sparkline(cairo_t* cr, const Rect& r, const std::vector<float>& values, const Color& c, bool filled = true);
void divider(cairo_t* cr, float x1, float y1, float x2, float y2, const Color& c);

// Scales the current transform around (cx, cy).
void scale_around(cairo_t* cr, float cx, float cy, float s);

void reset_fonts();

}  // namespace island::paint
