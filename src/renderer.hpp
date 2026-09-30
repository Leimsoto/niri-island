#pragma once

#include <cairo.h>
#include <pango/pangocairo.h>
#include <string>
#include "mpris.hpp"
#include "sysmon.hpp"
#include "workspaces.hpp"

namespace island {

enum class IslandMode {
    Idle,
    IdleExpanded,
    MusicCompact,
    MusicExpanded,
    ControlCenter,
    HudOSD,
    Notification
};

struct OsdState;
struct NotificationItem;

struct ControlCenterState {
    bool wifi_enabled = true;
    std::string wifi_ssid = "Wi-Fi";
    bool bluetooth_enabled = true;
    bool audio_muted = false;
    float volume = 0.5f;       // 0.0 to 1.0
    float brightness = 0.8f;   // 0.0 to 1.0
    int battery_percent = 100;
    bool battery_charging = false;
    bool dnd_enabled = false;
    bool media_active = false;
    SystemStats stats;

    // Press depth and bounce animation values
    float volume_press = 0.0f;
    float volume_bounce = 0.0f;
    float brightness_press = 0.0f;
    float brightness_bounce = 0.0f;

    // Interactive Calendar & Notification State
    int cal_month_offset = 0;
    float notif_expand = 0.0f;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    void draw(cairo_t* cr, float width, float height, IslandMode mode,
               const MediaInfo& media, const OsdState& osd,
               const NotificationItem& notif, const ControlCenterState& cc_state,
               bool mic_active, float anim_time,
               float ws_bubble_w, const std::vector<WorkspaceInfo>& workspaces, uint32_t active_ws_idx,
               float ws_pill_pos = 0.0f, float ws_pill_vel = 0.0f, float ws_bounce = 0.0f);

private:
    void draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r);
    void draw_workspace_bubble(cairo_t* cr, float x, float y, float w, float h,
                              const std::vector<WorkspaceInfo>& workspaces, uint32_t active_idx,
                              float pill_pos, float pill_vel, float pill_bounce);
    void draw_idle(cairo_t* cr, float w, float h);
    void draw_idle_expanded(cairo_t* cr, float w, float h, const ControlCenterState& cc_state);
    void draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
    void draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time);
    void draw_control_center(cairo_t* cr, float w, float h, const ControlCenterState& state,
                             const MediaInfo& media, const NotificationItem& notif,
                             bool mic_active, float anim_time);
    void draw_calendar(cairo_t* cr, float x, float y, float w, float h, struct tm* tm_info, int month_offset);
    void draw_weather_card(cairo_t* cr, float x, float y, float w, float h);
    void draw_hud_osd(cairo_t* cr, float w, float h, const OsdState& osd);
    void draw_notification(cairo_t* cr, float w, float h, const NotificationItem& notif,
                           const MediaInfo& media, float anim_time);
    void draw_privacy_dot(cairo_t* cr, float w, float h);

    void update_art(const std::string& art_url);
    void update_wallpaper_thumb();
    static cairo_surface_t* load_image_scaled(const std::string& path_or_uri, int target_w, int target_h);

    std::string current_art_url_;
    cairo_surface_t* art_surface_ = nullptr;       // 50x50 for expanded
    cairo_surface_t* mini_art_surface_ = nullptr;  // 22x22 for compact
    std::string current_wallpaper_path_;
    cairo_surface_t* wallpaper_thumb_ = nullptr;
};

} // namespace island
