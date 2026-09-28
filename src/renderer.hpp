#pragma once

#include <cairo.h>
#include <pango/pangocairo.h>
#include <string>
#include "mpris.hpp"

namespace island {

enum class IslandMode {
    Idle,
    MusicCompact,
    MusicExpanded,
    HudOSD,
    Notification
};

struct OsdState;
struct NotificationItem;

class Renderer {
public:
    Renderer();
    ~Renderer();

    void draw(cairo_t* cr, float width, float height, IslandMode mode,
               const MediaInfo& media, const OsdState& osd,
               const NotificationItem& notif, bool mic_active, float anim_time);

private:
    void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);
    void draw_idle(cairo_t* cr, float w, float h);
    void draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
    void draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
    void draw_hud_osd(cairo_t* cr, float w, float h, const OsdState& osd);
    void draw_notification(cairo_t* cr, float w, float h, const NotificationItem& notif);
    void draw_privacy_dot(cairo_t* cr, float w, float h);

    void update_art(const std::string& art_url);
    static cairo_surface_t* load_image_scaled(const std::string& path_or_uri, int target_w, int target_h);

    std::string current_art_url_;
    cairo_surface_t* art_surface_ = nullptr;       // 50x50 for expanded
    cairo_surface_t* mini_art_surface_ = nullptr;  // 22x22 for compact
};

} // namespace island
