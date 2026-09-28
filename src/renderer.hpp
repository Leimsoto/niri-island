#pragma once

#include <cairo.h>
#include <pango/pangocairo.h>
#include <string>
#include "mpris.hpp"

namespace island {

enum class IslandMode {
    Idle,
    MusicCompact,
    MusicExpanded
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    void draw(cairo_t* cr, float width, float height, IslandMode mode, const MediaInfo& media, float anim_time);

private:
    void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);
    void draw_idle(cairo_t* cr, float w, float h);
    void draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
    void draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
};

} // namespace island
