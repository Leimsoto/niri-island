#include "renderer.hpp"
#include "osd.hpp"
#include "notifications.hpp"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cmath>
#include <ctime>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>

namespace island {

Renderer::Renderer() = default;

Renderer::~Renderer() {
    if (art_surface_) {
        cairo_surface_destroy(art_surface_);
        art_surface_ = nullptr;
    }
    if (mini_art_surface_) {
        cairo_surface_destroy(mini_art_surface_);
        mini_art_surface_ = nullptr;
    }
    if (wallpaper_thumb_) {
        cairo_surface_destroy(wallpaper_thumb_);
        wallpaper_thumb_ = nullptr;
    }
}

static std::string resolve_art_path(const std::string& path_or_uri) {
    if (path_or_uri.empty()) return "";
    if (path_or_uri.rfind("file://", 0) == 0) {
        char* local = g_filename_from_uri(path_or_uri.c_str(), nullptr, nullptr);
        if (local) {
            std::string res = local;
            g_free(local);
            return res;
        }
        return path_or_uri.substr(7);
    }
    if (path_or_uri.rfind("http://", 0) == 0 || path_or_uri.rfind("https://", 0) == 0) {
        std::hash<std::string> hasher;
        std::string cache_path = "/tmp/niri_art_" + std::to_string(hasher(path_or_uri)) + ".img";
        if (std::filesystem::exists(cache_path) && std::filesystem::file_size(cache_path) > 0) {
            return cache_path;
        }
        static std::string last_download;
        if (last_download != path_or_uri) {
            last_download = path_or_uri;
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "curl -s -L '%s' -o '%s' &", path_or_uri.c_str(), cache_path.c_str());
            int r = system(cmd);
            (void)r;
        }
        return "";
    }
    return path_or_uri;
}

cairo_surface_t* Renderer::load_image_scaled(const std::string& path_or_uri, int target_w, int target_h) {
    if (path_or_uri.empty()) return nullptr;

    std::string path = resolve_art_path(path_or_uri);
    if (path.empty()) return nullptr;

    GError* err = nullptr;
    GdkPixbuf* pixbuf = gdk_pixbuf_new_from_file_at_scale(path.c_str(), target_w, target_h, FALSE, &err);
    if (!pixbuf) {
        if (err) g_error_free(err);
        return nullptr;
    }

    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        g_object_unref(pixbuf);
        return nullptr;
    }

    unsigned char* dst_data = cairo_image_surface_get_data(surface);
    const guchar* src_pixels = gdk_pixbuf_read_pixels(pixbuf);
    int src_rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    int n_channels = gdk_pixbuf_get_n_channels(pixbuf);

    for (int y = 0; y < height; ++y) {
        const guchar* src = src_pixels + y * src_rowstride;
        uint32_t* dst = reinterpret_cast<uint32_t*>(dst_data + y * stride);
        for (int x = 0; x < width; ++x) {
            uint8_t r = src[0];
            uint8_t g = src[1];
            uint8_t b = src[2];
            uint8_t a = (n_channels == 4) ? src[3] : 255;
            src += n_channels;

            if (a == 0) {
                *dst++ = 0;
            } else if (a == 255) {
                *dst++ = (255u << 24) | (r << 16) | (g << 8) | b;
            } else {
                uint8_t pr = (r * a + 127) / 255;
                uint8_t pg = (g * a + 127) / 255;
                uint8_t pb = (b * a + 127) / 255;
                *dst++ = (static_cast<uint32_t>(a) << 24) | (pr << 16) | (pg << 8) | pb;
            }
        }
    }

    cairo_surface_mark_dirty(surface);
    g_object_unref(pixbuf);
    return surface;
}

void Renderer::update_art(const std::string& art_url) {
    if (art_url.empty()) {
        if (!current_art_url_.empty() || art_surface_ || mini_art_surface_) {
            current_art_url_.clear();
            if (art_surface_) {
                cairo_surface_destroy(art_surface_);
                art_surface_ = nullptr;
            }
            if (mini_art_surface_) {
                cairo_surface_destroy(mini_art_surface_);
                mini_art_surface_ = nullptr;
            }
        }
        return;
    }

    if (art_url == current_art_url_ && art_surface_ && mini_art_surface_) return;

    current_art_url_ = art_url;
    if (art_surface_) {
        cairo_surface_destroy(art_surface_);
        art_surface_ = nullptr;
    }
    if (mini_art_surface_) {
        cairo_surface_destroy(mini_art_surface_);
        mini_art_surface_ = nullptr;
    }

    art_surface_ = load_image_scaled(art_url, 48, 48);
    mini_art_surface_ = load_image_scaled(art_url, 22, 22);
}

void Renderer::update_wallpaper_thumb() {
    const char* home = getenv("HOME");
    if (!home) return;
    std::string config_path = std::string(home) + "/.cache/skwd-wall-v2/last-wallpaper.json";
    if (!std::filesystem::exists(config_path)) return;

    try {
        std::ifstream f(config_path);
        if (!f.is_open()) return;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t pos = content.find("\"path\":\"");
        if (pos == std::string::npos) return;
        pos += 8;
        size_t end_pos = content.find("\"", pos);
        if (end_pos == std::string::npos) return;
        std::string wall_path = content.substr(pos, end_pos - pos);

        if (wall_path == current_wallpaper_path_ && wallpaper_thumb_) return;

        if (wallpaper_thumb_) {
            cairo_surface_destroy(wallpaper_thumb_);
            wallpaper_thumb_ = nullptr;
        }
        current_wallpaper_path_ = wall_path;
        wallpaper_thumb_ = load_image_scaled(wall_path, 252, 202);
    } catch (...) {}
}

void Renderer::draw_rounded_rect(cairo_t* cr, double x, double y, double w, double h, double r) {
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void Renderer::draw(cairo_t* cr, float width, float height, IslandMode mode,
                    const MediaInfo& media, const OsdState& osd,
                    const NotificationItem& notif, const ControlCenterState& cc_state,
                    bool mic_active, float anim_time,
                    float ws_bubble_w, const std::vector<WorkspaceInfo>& workspaces, uint32_t active_ws_idx) {
    if (media.active && !media.title.empty()) {
        update_art(media.art_url);
    } else {
        update_art("");
    }

    // 1. Calculate symmetrical side wings so the main island is perfectly centered horizontally
    float effective_ws_w = (mode == IslandMode::ControlCenter) ? 0.0f : ws_bubble_w;
    float left_wing = (effective_ws_w > 0.0f) ? (effective_ws_w + 8.0f) : 0.0f;
    float right_wing = 0.0f;
    float wing = std::max(left_wing, right_wing);

    float main_x = wing;
    float main_w = std::max(20.0f, width - 2.0f * wing);

    if (effective_ws_w > 0.0f) {
        float ws_x = wing - 8.0f - effective_ws_w;
        draw_workspace_bubble(cr, ws_x, 0.0f, effective_ws_w, 32.0f, workspaces, active_ws_idx);
    }

    double radius = std::min((double)height / 2.0, 24.0);

    // 2. Draw Main Island Body (Material 3 Dark Surface #141218 - Pure seamless, no outer outline)
    draw_rounded_rect(cr, main_x, 0.0, main_w, height, radius);
    cairo_set_source_rgba(cr, 0.078, 0.071, 0.094, 0.98); // M3 Surface Container Lowest (#141218)
    cairo_fill(cr);

    // Clip content to rounded pill
    cairo_save(cr);
    cairo_translate(cr, main_x, 0.0);
    draw_rounded_rect(cr, 0.0, 0.0, main_w, height, radius);
    cairo_clip(cr);

    // 3. Draw Mode Content
    switch (mode) {
        case IslandMode::Idle:
            draw_idle(cr, main_w, height);
            break;
        case IslandMode::IdleExpanded:
            draw_idle_expanded(cr, main_w, height, cc_state);
            break;
        case IslandMode::MusicCompact:
            draw_music_compact(cr, main_w, height, media, anim_time);
            break;
        case IslandMode::MusicExpanded:
            draw_music_expanded(cr, main_w, height, media, anim_time);
            break;
        case IslandMode::ControlCenter:
            update_wallpaper_thumb();
            draw_control_center(cr, main_w, height, cc_state, media, notif, mic_active, anim_time);
            break;
        case IslandMode::HudOSD:
            draw_hud_osd(cr, main_w, height, osd);
            break;
        case IslandMode::Notification:
            draw_notification(cr, main_w, height, notif);
            break;
    }

    cairo_restore(cr);
}

void Renderer::draw_workspace_bubble(cairo_t* cr, float x, float y, float w, float h,
                                     const std::vector<WorkspaceInfo>& workspaces, uint32_t active_idx) {
    double radius = std::min((double)h / 2.0, 16.0);
    draw_rounded_rect(cr, x, y, w, h, radius);
    cairo_set_source_rgba(cr, 0.078, 0.071, 0.094, 0.98); // M3 Dark Surface #141218
    cairo_fill(cr);

    if (workspaces.empty()) return;

    size_t count = workspaces.size();
    float dot_size = 6.0f;
    float active_pill_w = 18.0f;
    float gap = 6.0f;
    float total_content_w = 0.0f;

    for (const auto& ws : workspaces) {
        bool is_act = (ws.idx == active_idx || ws.is_active || ws.is_focused);
        total_content_w += is_act ? active_pill_w : dot_size;
    }
    total_content_w += (count - 1) * gap;

    float cur_x = x + (w - total_content_w) / 2.0f;
    float center_y = y + h / 2.0f;

    for (const auto& ws : workspaces) {
        bool is_act = (ws.idx == active_idx || ws.is_active || ws.is_focused);
        if (is_act) {
            // Active elongated capsule in M3 Primary Lavender
            draw_rounded_rect(cr, cur_x, center_y - 3.0f, active_pill_w, 6.0f, 3.0);
            cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.95);
            cairo_fill(cr);
            cur_x += active_pill_w + gap;
        } else {
            // Inactive subtle dot in M3 Muted Gray
            cairo_arc(cr, cur_x + dot_size / 2.0f, center_y, dot_size / 2.0f, 0, 2 * M_PI);
            cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.65);
            cairo_fill(cr);
            cur_x += dot_size + gap;
        }
    }
}

void Renderer::draw_privacy_dot(cairo_t*, float, float) {
    // Disabled per user preference (no unwanted dots)
}

void Renderer::draw_idle(cairo_t* cr, float w, float h) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("JetBrainsMono Nerd Font SemiBold 11");
    if (!font_desc) font_desc = pango_font_description_from_string("Sans SemiBold 11");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, time_str, -1);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);

    // Clean, perfectly centered clock without any extra dots
    double text_x = (w - ink_rect.width) / 2.0 - ink_rect.x;
    double text_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.98);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_idle_expanded(cairo_t* cr, float, float h, const ControlCenterState& cc_state) {
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    char time_str[16];
    char sec_str[8];
    char day_name[32];
    char date_full[48];

    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    strftime(sec_str, sizeof(sec_str), "%S", tm_info);
    strftime(day_name, sizeof(day_name), "%A", tm_info);
    strftime(date_full, sizeof(date_full), "%d de %B", tm_info);

    // Left: Big Digital Clock
    PangoLayout* clk_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* clk_font = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 18");
    pango_layout_set_font_description(clk_layout, clk_font);
    pango_layout_set_text(clk_layout, time_str, -1);
    PangoRectangle clk_ink, clk_log;
    pango_layout_get_pixel_extents(clk_layout, &clk_ink, &clk_log);
    float clk_x = 20.0f;
    float clk_y = (h - clk_ink.height) / 2.0f - clk_ink.y;
    cairo_move_to(cr, clk_x, clk_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 1.0); // M3 On-Surface
    pango_cairo_show_layout(cr, clk_layout);
    pango_font_description_free(clk_font);
    g_object_unref(clk_layout);

    // Small Seconds next to clock
    PangoLayout* sec_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* sec_font = pango_font_description_from_string("JetBrainsMono Nerd Font 9.5");
    pango_layout_set_font_description(sec_layout, sec_font);
    pango_layout_set_text(sec_layout, sec_str, -1);
    cairo_move_to(cr, clk_x + clk_ink.width + 4.0f, clk_y + 4.0f);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.85); // M3 Outline
    pango_cairo_show_layout(cr, sec_layout);
    pango_font_description_free(sec_font);
    g_object_unref(sec_layout);

    // Subtle Vertical Divider
    float div_x = clk_x + clk_ink.width + 28.0f;
    cairo_move_to(cr, div_x, 14.0f);
    cairo_line_to(cr, div_x, h - 14.0f);
    cairo_set_source_rgba(cr, 0.40, 0.36, 0.48, 0.25);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Right: Date & System Glance
    float info_x = div_x + 14.0f;

    // Day of week & date
    PangoLayout* day_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* day_font = pango_font_description_from_string("Sans Bold 10.5");
    pango_layout_set_font_description(day_layout, day_font);
    pango_layout_set_text(day_layout, day_name, -1);
    cairo_move_to(cr, info_x, 14.0f);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.98);
    pango_cairo_show_layout(cr, day_layout);
    pango_font_description_free(day_font);
    g_object_unref(day_layout);

    PangoLayout* dt_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* dt_font = pango_font_description_from_string("Sans 9.5");
    pango_layout_set_font_description(dt_layout, dt_font);
    pango_layout_set_text(dt_layout, date_full, -1);
    cairo_move_to(cr, info_x, 30.0f);
    cairo_set_source_rgba(cr, 0.792, 0.769, 0.816, 0.85);
    pango_cairo_show_layout(cr, dt_layout);
    pango_font_description_free(dt_font);
    g_object_unref(dt_layout);

    // Minimal system status line: Battery & CPU
    std::string sys_info = "󰁹 " + std::to_string(cc_state.battery_percent) + "%  •  󰍛 " + 
                           std::to_string(static_cast<int>(std::round(cc_state.stats.cpu_percent))) + "%";
    PangoLayout* sys_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* sys_font = pango_font_description_from_string("JetBrainsMono Nerd Font 8.5");
    pango_layout_set_font_description(sys_layout, sys_font);
    pango_layout_set_text(sys_layout, sys_info.c_str(), -1);
    cairo_move_to(cr, info_x, 46.0f);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.80);
    pango_cairo_show_layout(cr, sys_layout);
    pango_font_description_free(sys_font);
    g_object_unref(sys_layout);
}

void Renderer::draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time) {
    // 1. Center: Clock (HH:MM)
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("JetBrainsMono Nerd Font SemiBold 11");
    if (!font_desc) font_desc = pango_font_description_from_string("Sans SemiBold 11");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, time_str, -1);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);

    double clock_x = (w - ink_rect.width) / 2.0 - ink_rect.x;
    double clock_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, clock_x, clock_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.98);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);

    // 2. Left side: Square Album Art with rounded corners
    float art_size = 20.0f;
    float art_x = 7.0f;
    float art_y = (h - art_size) / 2.0f;
    float art_radius = 4.5f;

    if (mini_art_surface_) {
        cairo_save(cr);
        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, art_radius);
        cairo_clip(cr);
        cairo_set_source_surface(cr, mini_art_surface_, art_x, art_y);
        cairo_paint(cr);
        cairo_restore(cr);

        // Subtle Material border highlight
        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, art_radius);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.25);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);
    } else {
        // Fallback: stylish dark container with music note
        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, art_radius);
        cairo_set_source_rgba(cr, 0.18, 0.16, 0.22, 0.95);
        cairo_fill(cr);

        PangoLayout* icon_l = pango_cairo_create_layout(cr);
        PangoFontDescription* icon_f = pango_font_description_from_string("JetBrainsMono Nerd Font 9");
        pango_layout_set_font_description(icon_l, icon_f);
        pango_layout_set_text(icon_l, "󰎆", -1);
        PangoRectangle ink_i, log_i;
        pango_layout_get_pixel_extents(icon_l, &ink_i, &log_i);
        cairo_move_to(cr, art_x + (art_size - ink_i.width) / 2.0 - ink_i.x, 
                          art_y + (art_size - ink_i.height) / 2.0 - ink_i.y);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.85);
        pango_cairo_show_layout(cr, icon_l);
        pango_font_description_free(icon_f);
        g_object_unref(icon_l);
    }

    // 3. Right side: Mini Cava-style equalizer bars animation
    float cava_w = 18.0f;
    float cava_x = w - 8.0f - cava_w;
    float cava_center_y = h / 2.0f;
    int num_bars = 4;
    float bar_w = 2.2f;
    float bar_spacing = (cava_w - num_bars * bar_w) / (num_bars - 1);

    float freqs[4] = { 6.2f, 8.5f, 7.1f, 9.3f };
    float phases[4] = { 0.2f, 1.5f, 3.1f, 4.4f };

    for (int i = 0; i < num_bars; ++i) {
        float bx = cava_x + i * (bar_w + bar_spacing);
        float bar_h = 3.0f;
        if (media.playing) {
            float wave = 0.5f + 0.35f * std::sin(anim_time * freqs[i] + phases[i])
                              + 0.15f * std::cos(anim_time * 12.0f + phases[i] * 1.8f);
            bar_h = 3.5f + 11.5f * std::clamp(wave, 0.0f, 1.0f);
        }

        draw_rounded_rect(cr, bx, cava_center_y - bar_h / 2.0f, bar_w, bar_h, 1.1);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, media.playing ? 0.92 : 0.45); // Subtle when paused
        cairo_fill(cr);
    }
}

void Renderer::draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float) {
    (void)h;
    float art_x = 16.0f;
    float art_y = 14.0f;
    float art_size = 48.0f;

    // 1. Album Art box (Material rounded square with subtle border & glass effect)
    cairo_save(cr);
    draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 10.0);
    cairo_clip(cr);

    if (art_surface_) {
        cairo_set_source_surface(cr, art_surface_, art_x, art_y);
        cairo_paint(cr);
    } else {
        cairo_pattern_t* pat = cairo_pattern_create_linear(art_x, art_y, art_x + art_size, art_y + art_size);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.22, 0.18, 0.30, 0.95);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.12, 0.10, 0.18, 0.95);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);

        PangoLayout* art_icon = pango_cairo_create_layout(cr);
        PangoFontDescription* art_font = pango_font_description_from_string("JetBrainsMono Nerd Font 16");
        pango_layout_set_font_description(art_icon, art_font);
        pango_layout_set_text(art_icon, "󰎆", -1);

        PangoRectangle ink_r, log_r;
        pango_layout_get_pixel_extents(art_icon, &ink_r, &log_r);
        cairo_move_to(cr, art_x + (art_size - ink_r.width) / 2.0 - ink_r.x,
                          art_y + (art_size - ink_r.height) / 2.0 - ink_r.y);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.90);
        pango_cairo_show_layout(cr, art_icon);
        pango_font_description_free(art_font);
        g_object_unref(art_icon);
    }
    cairo_restore(cr);

    draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 10.0);
    cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // 2. Impasto IslandSummary Top Right Header: Clock, Date & CC Button
    float cc_btn_x = w - 34.0f;
    float cc_btn_y = 14.0f;
    float cc_btn_size = 22.0f;
    draw_rounded_rect(cr, cc_btn_x, cc_btn_y, cc_btn_size, cc_btn_size, 11.0);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.90); // Surface Container High
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.35);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    PangoLayout* cc_icon = pango_cairo_create_layout(cr);
    PangoFontDescription* cc_font = pango_font_description_from_string("JetBrainsMono Nerd Font 10");
    pango_layout_set_font_description(cc_icon, cc_font);
    pango_layout_set_text(cc_icon, "󰕮", -1);
    PangoRectangle cc_ink, cc_log;
    pango_layout_get_pixel_extents(cc_icon, &cc_ink, &cc_log);
    cairo_move_to(cr, cc_btn_x + (cc_btn_size - cc_ink.width) / 2.0 - cc_ink.x,
                      cc_btn_y + (cc_btn_size - cc_ink.height) / 2.0 - cc_ink.y);
    cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.90);
    pango_cairo_show_layout(cr, cc_icon);
    pango_font_description_free(cc_font);
    g_object_unref(cc_icon);

    // Clock & Date Stack (Impasto style)
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_str[16];
    char date_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    strftime(date_str, sizeof(date_str), "%a %d", tm_info);

    PangoLayout* clk_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* clk_font = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 10");
    pango_layout_set_font_description(clk_layout, clk_font);
    pango_layout_set_text(clk_layout, time_str, -1);
    PangoRectangle clk_ink, clk_log;
    pango_layout_get_pixel_extents(clk_layout, &clk_ink, &clk_log);
    float clk_x = cc_btn_x - clk_ink.width - 10.0f;
    cairo_move_to(cr, clk_x, 14.0f - clk_ink.y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
    pango_cairo_show_layout(cr, clk_layout);
    pango_font_description_free(clk_font);
    g_object_unref(clk_layout);

    PangoLayout* dt_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* dt_font = pango_font_description_from_string("Sans 8.0");
    pango_layout_set_font_description(dt_layout, dt_font);
    pango_layout_set_text(dt_layout, date_str, -1);
    PangoRectangle dt_ink, dt_log;
    pango_layout_get_pixel_extents(dt_layout, &dt_ink, &dt_log);
    cairo_move_to(cr, cc_btn_x - dt_ink.width - 10.0f, 26.0f - dt_ink.y);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.85);
    pango_cairo_show_layout(cr, dt_layout);
    pango_font_description_free(dt_font);
    g_object_unref(dt_layout);

    // 3. Song Title & Artist in Material 3
    float info_x = art_x + art_size + 12.0f;
    float info_w = clk_x - info_x - 8.0f;
    if (info_w < 50.0f) info_w = 50.0f;

    PangoLayout* title_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* title_font = pango_font_description_from_string("Sans Bold 10.5");
    pango_layout_set_font_description(title_layout, title_font);
    pango_layout_set_text(title_layout, media.title.empty() ? "Sin reproducción" : media.title.c_str(), -1);
    pango_layout_set_ellipsize(title_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(title_layout, info_w * PANGO_SCALE);

    PangoRectangle title_ink, title_log;
    pango_layout_get_pixel_extents(title_layout, &title_ink, &title_log);
    double title_y = art_y + 6.0 - title_ink.y;

    cairo_move_to(cr, info_x, title_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 1.0); // M3 On-Surface
    pango_cairo_show_layout(cr, title_layout);
    pango_font_description_free(title_font);
    g_object_unref(title_layout);

    PangoLayout* artist_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* artist_font = pango_font_description_from_string("Sans 9.5");
    pango_layout_set_font_description(artist_layout, artist_font);
    pango_layout_set_text(artist_layout, media.artist.empty() ? "Desconocido" : media.artist.c_str(), -1);
    pango_layout_set_ellipsize(artist_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(artist_layout, info_w * PANGO_SCALE);

    PangoRectangle artist_ink, artist_log;
    pango_layout_get_pixel_extents(artist_layout, &artist_ink, &artist_log);
    double artist_y = art_y + 25.0 - artist_ink.y;

    cairo_move_to(cr, info_x, artist_y);
    cairo_set_source_rgba(cr, 0.792, 0.769, 0.816, 0.85); // M3 Muted
    pango_cairo_show_layout(cr, artist_layout);
    pango_font_description_free(artist_font);
    g_object_unref(artist_layout);

    // 4. Impasto / M3 Capsule Progress Slider (LevelRow style)
    float bar_x = 16.0f;
    float bar_y = 68.0f;
    float bar_w = w - 32.0f;
    float bar_h = 7.0f;

    // Track
    draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 3.5);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.95); // Surface Container High
    cairo_fill(cr);

    float fill_ratio = 0.0f;
    if (media.length_us > 0) {
        fill_ratio = std::clamp(static_cast<float>(media.position_us) / static_cast<float>(media.length_us), 0.0f, 1.0f);
    }

    if (fill_ratio > 0.005f) {
        float active_w = std::max(7.0f, bar_w * fill_ratio);
        draw_rounded_rect(cr, bar_x, bar_y, active_w, bar_h, 3.5);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.98); // M3 Lavender Fill
        cairo_fill(cr);

        // Tactile Thumb handle
        cairo_arc(cr, bar_x + active_w - 3.5f, bar_y + bar_h / 2.0f, 4.0, 0, 2 * M_PI);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_fill(cr);
    }

    // 5. Time Timestamps (Elapsed & Total)
    int64_t cur_sec = media.position_us / 1000000;
    int64_t tot_sec = media.length_us / 1000000;

    std::ostringstream cur_oss, tot_oss;
    cur_oss << std::setfill('0') << std::setw(2) << (cur_sec / 60) << ":"
            << std::setfill('0') << std::setw(2) << (cur_sec % 60);
    tot_oss << std::setfill('0') << std::setw(2) << (tot_sec / 60) << ":"
            << std::setfill('0') << std::setw(2) << (tot_sec % 60);

    PangoFontDescription* time_font = pango_font_description_from_string("JetBrainsMono Nerd Font 8.0");

    // Elapsed
    PangoLayout* cur_time_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(cur_time_layout, time_font);
    pango_layout_set_text(cur_time_layout, cur_oss.str().c_str(), -1);
    cairo_move_to(cr, bar_x, bar_y + 9.0);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.90);
    pango_cairo_show_layout(cr, cur_time_layout);
    g_object_unref(cur_time_layout);

    // Total
    PangoLayout* tot_time_layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(tot_time_layout, time_font);
    pango_layout_set_text(tot_time_layout, tot_oss.str().c_str(), -1);
    PangoRectangle tot_ink, tot_log;
    pango_layout_get_pixel_extents(tot_time_layout, &tot_ink, &tot_log);
    cairo_move_to(cr, bar_x + bar_w - tot_ink.width - tot_ink.x, bar_y + 9.0);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.90);
    pango_cairo_show_layout(cr, tot_time_layout);
    g_object_unref(tot_time_layout);
    pango_font_description_free(time_font);

    // 6. Media Control Buttons (Material 3 Circular Tonal Chips)
    PangoLayout* btn_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* btn_font = pango_font_description_from_string("JetBrainsMono Nerd Font 13");
    pango_layout_set_font_description(btn_layout, btn_font);

    double btn_center_y = 107.0;

    // Previous Button
    float prev_x = w / 2.0f - 50.0f;
    draw_rounded_rect(cr, prev_x - 14.0f, btn_center_y - 14.0f, 28.0f, 28.0f, 14.0);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.85);
    cairo_fill(cr);

    pango_layout_set_text(btn_layout, "󰒮", -1);
    PangoRectangle prev_ink, prev_log;
    pango_layout_get_pixel_extents(btn_layout, &prev_ink, &prev_log);
    cairo_move_to(cr, prev_x - prev_ink.width / 2.0 - prev_ink.x,
                      btn_center_y - prev_ink.height / 2.0 - prev_ink.y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
    pango_cairo_show_layout(cr, btn_layout);

    // Play/Pause Button (Prominent M3 Primary Container #4f378b)
    float play_x = w / 2.0f;
    draw_rounded_rect(cr, play_x - 17.0f, btn_center_y - 17.0f, 34.0f, 34.0f, 17.0);
    cairo_set_source_rgba(cr, 0.310, 0.216, 0.545, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.40);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    pango_layout_set_text(btn_layout, media.playing ? "󰏤" : "󰐊", -1);
    PangoRectangle play_ink, play_log;
    pango_layout_get_pixel_extents(btn_layout, &play_ink, &play_log);
    cairo_move_to(cr, play_x - play_ink.width / 2.0 - play_ink.x,
                      btn_center_y - play_ink.height / 2.0 - play_ink.y);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    pango_cairo_show_layout(cr, btn_layout);

    // Next Button
    float next_x = w / 2.0f + 50.0f;
    draw_rounded_rect(cr, next_x - 14.0f, btn_center_y - 14.0f, 28.0f, 28.0f, 14.0);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.85);
    cairo_fill(cr);

    pango_layout_set_text(btn_layout, "󰒭", -1);
    PangoRectangle next_ink, next_log;
    pango_layout_get_pixel_extents(btn_layout, &next_ink, &next_log);
    cairo_move_to(cr, next_x - next_ink.width / 2.0 - next_ink.x,
                      btn_center_y - next_ink.height / 2.0 - next_ink.y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
    pango_cairo_show_layout(cr, btn_layout);

    pango_font_description_free(btn_font);
    g_object_unref(btn_layout);
}

void Renderer::draw_calendar(cairo_t* cr, float x, float y, float w, float h, struct tm* tm_info) {
    draw_rounded_rect(cr, x, y, w, h, 14.0);
    cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.96); // #16151b
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.20);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    static const char* months_en[] = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"
    };
    static const char* days_en[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };

    int cur_year = tm_info->tm_year + 1900;
    int cur_mon = tm_info->tm_mon;
    int cur_day = tm_info->tm_mday;
    int cur_wday = tm_info->tm_wday;

    // Header
    std::string header_str = std::string(months_en[cur_mon]) + " " + std::to_string(cur_year);
    PangoLayout* hl = pango_cairo_create_layout(cr);
    PangoFontDescription* hf = pango_font_description_from_string("Sans Bold 10.5");
    pango_layout_set_font_description(hl, hf);
    pango_layout_set_text(hl, header_str.c_str(), -1);
    cairo_move_to(cr, x + 14.0, y + 10.0);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 0.98);
    pango_cairo_show_layout(cr, hl);
    pango_font_description_free(hf);
    g_object_unref(hl);

    // Day of week + Arrows
    std::string day_str = days_en[cur_wday];
    PangoLayout* dl = pango_cairo_create_layout(cr);
    PangoFontDescription* df = pango_font_description_from_string("Sans SemiBold 9.0");
    pango_layout_set_font_description(dl, df);
    pango_layout_set_text(dl, day_str.c_str(), -1);
    PangoRectangle d_ink, d_log;
    pango_layout_get_pixel_extents(dl, &d_ink, &d_log);
    cairo_move_to(cr, x + w - 48.0 - d_ink.width, y + 11.0);
    cairo_set_source_rgba(cr, 0.38, 0.54, 0.85, 0.95);
    pango_cairo_show_layout(cr, dl);
    pango_font_description_free(df);
    g_object_unref(dl);

    PangoLayout* arr = pango_cairo_create_layout(cr);
    PangoFontDescription* af = pango_font_description_from_string("JetBrainsMono Nerd Font 9.5");
    pango_layout_set_font_description(arr, af);
    pango_layout_set_text(arr, "󰅁  󰅂", -1);
    cairo_move_to(cr, x + w - 40.0, y + 11.0);
    cairo_set_source_rgba(cr, 0.65, 0.65, 0.70, 0.85);
    pango_cairo_show_layout(cr, arr);
    pango_font_description_free(af);
    g_object_unref(arr);

    // Weekdays
    const char* weekdays[] = {"M", "T", "W", "T", "F", "S", "S"};
    float grid_x = x + 14.0f;
    float grid_w = w - 28.0f;
    float col_w = grid_w / 7.0f;
    float row_y = y + 36.0f;

    PangoLayout* wl = pango_cairo_create_layout(cr);
    PangoFontDescription* wf = pango_font_description_from_string("Sans Bold 8.5");
    pango_layout_set_font_description(wl, wf);
    for (int i = 0; i < 7; ++i) {
        pango_layout_set_text(wl, weekdays[i], -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(wl, &ink, &log);
        cairo_move_to(cr, grid_x + i * col_w + (col_w - ink.width) / 2.0 - ink.x, row_y);
        cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.80);
        pango_cairo_show_layout(cr, wl);
    }
    pango_font_description_free(wf);
    g_object_unref(wl);

    // Days calculation
    int days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if ((cur_year % 4 == 0 && cur_year % 100 != 0) || (cur_year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    struct tm first_tm = *tm_info;
    first_tm.tm_mday = 1;
    mktime(&first_tm);
    int start_col = (first_tm.tm_wday + 6) % 7;

    int prev_mon = (cur_mon + 11) % 12;
    int prev_max_days = days_in_month[prev_mon];

    float start_grid_y = row_y + 18.0f;
    float row_h = 23.0f;

    PangoLayout* nl = pango_cairo_create_layout(cr);
    PangoFontDescription* nf = pango_font_description_from_string("Sans 8.5");
    PangoFontDescription* nf_bold = pango_font_description_from_string("Sans Bold 8.5");

    int cur_day_num = 1;
    int next_day_num = 1;

    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 7; ++c) {
            float cell_cx = grid_x + c * col_w + col_w / 2.0f;
            float cell_cy = start_grid_y + r * row_h + row_h / 2.0f;

            int cell_val = 0;
            bool is_current_month = false;
            bool is_today = false;

            if (r == 0 && c < start_col) {
                cell_val = prev_max_days - start_col + 1 + c;
            } else if (cur_day_num <= days_in_month[cur_mon]) {
                cell_val = cur_day_num;
                is_current_month = true;
                if (cell_val == cur_day) is_today = true;
                cur_day_num++;
            } else {
                cell_val = next_day_num++;
            }

            std::string val_str = std::to_string(cell_val);

            if (is_today) {
                draw_rounded_rect(cr, cell_cx - 10.5, cell_cy - 10.5, 21.0, 21.0, 10.5);
                cairo_set_source_rgba(cr, 0.23, 0.35, 0.60, 0.95);
                cairo_fill(cr);

                cairo_arc(cr, cell_cx, cell_cy + 13.0, 1.2, 0, 2 * M_PI);
                cairo_set_source_rgba(cr, 0.45, 0.65, 0.95, 0.95);
                cairo_fill(cr);

                pango_layout_set_font_description(nl, nf_bold);
                pango_layout_set_text(nl, val_str.c_str(), -1);
                PangoRectangle ink, log;
                pango_layout_get_pixel_extents(nl, &ink, &log);
                cairo_move_to(cr, cell_cx - ink.width / 2.0 - ink.x, cell_cy - ink.height / 2.0 - ink.y);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
                pango_cairo_show_layout(cr, nl);
            } else {
                pango_layout_set_font_description(nl, nf);
                pango_layout_set_text(nl, val_str.c_str(), -1);
                PangoRectangle ink, log;
                pango_layout_get_pixel_extents(nl, &ink, &log);
                cairo_move_to(cr, cell_cx - ink.width / 2.0 - ink.x, cell_cy - ink.height / 2.0 - ink.y);
                if (is_current_month) {
                    cairo_set_source_rgba(cr, 0.88, 0.86, 0.92, 0.92);
                } else {
                    cairo_set_source_rgba(cr, 0.42, 0.40, 0.46, 0.45);
                }
                pango_cairo_show_layout(cr, nl);
            }
        }
    }

    pango_font_description_free(nf);
    pango_font_description_free(nf_bold);
    g_object_unref(nl);
}

void Renderer::draw_weather_card(cairo_t* cr, float x, float y, float w, float h) {
    draw_rounded_rect(cr, x, y, w, h, 14.0);
    cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.96); // #16151b
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.20);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    std::string temp_str = "21°";
    std::string desc_str = "Overcast  •  feels 24°";
    std::string loc_str = "📍 Kyoto, Japan  •  7 min ago";
    std::string icon_str = "󰖐";
    std::string hi_lo_str = "29° / 21°";

    struct FHour { std::string time; std::string icon; std::string temp; std::string pop; };
    std::vector<FHour> fhours = {
        {"21:00", "󰖐", "22°", "40%"},
        {"00:00", "󰖐", "22°", "40%"},
        {"03:00", "󰖐", "22°", "40%"},
        {"06:00", "󰖐", "22°", "40%"}
    };

    const char* home = getenv("HOME");
    if (home) {
        std::string wjson = std::string(home) + "/.cache/noctalia/weather.json";
        std::ifstream wf(wjson);
        if (wf.is_open()) {
            std::string content((std::istreambuf_iterator<char>(wf)), std::istreambuf_iterator<char>());
            size_t t_pos = content.find("\"temperature_c\":");
            if (t_pos != std::string::npos) {
                float t = 0.0f;
                if (sscanf(content.c_str() + t_pos + 16, "%f", &t) == 1) {
                    temp_str = std::to_string(static_cast<int>(std::round(t))) + "°";
                }
            }
            float app_t = 0.0f;
            size_t at_pos = content.find("\"apparent_temperature_c\":");
            if (at_pos != std::string::npos) {
                if (sscanf(content.c_str() + at_pos + 25, "%f", &app_t) == 1) {
                    desc_str = "Soleado  •  sensación " + std::to_string(static_cast<int>(std::round(app_t))) + "°";
                }
            }
            size_t loc_pos = content.find("\"location_name\":");
            if (loc_pos != std::string::npos) {
                loc_pos += 16;
                while (loc_pos < content.size() && (content[loc_pos] == ' ' || content[loc_pos] == '\"')) loc_pos++;
                size_t loc_end = content.find("\"", loc_pos);
                if (loc_end != std::string::npos) {
                    loc_str = "📍 " + content.substr(loc_pos, loc_end - loc_pos);
                }
            }
            size_t c_pos = content.find("\"weather_code\":");
            if (c_pos != std::string::npos) {
                int code = 0;
                if (sscanf(content.c_str() + c_pos + 15, "%d", &code) == 1) {
                    if (code == 0) icon_str = "󰖙";
                    else if (code <= 3) icon_str = "󰖐";
                    else if (code >= 51 && code <= 65) icon_str = "󰖖";
                    else if (code >= 95) icon_str = "󰙾";
                }
            }
        }
    }

    // Top Weather Icon
    PangoLayout* il = pango_cairo_create_layout(cr);
    PangoFontDescription* ifont = pango_font_description_from_string("JetBrainsMono Nerd Font 24");
    pango_layout_set_font_description(il, ifont);
    pango_layout_set_text(il, icon_str.c_str(), -1);
    cairo_move_to(cr, x + 16.0, y + 10.0);
    cairo_set_source_rgba(cr, 0.38, 0.54, 0.85, 0.95);
    pango_cairo_show_layout(cr, il);
    pango_font_description_free(ifont);
    g_object_unref(il);

    // Big Temperature
    PangoLayout* tl = pango_cairo_create_layout(cr);
    PangoFontDescription* tf = pango_font_description_from_string("Sans Bold 20");
    pango_layout_set_font_description(tl, tf);
    pango_layout_set_text(tl, temp_str.c_str(), -1);
    cairo_move_to(cr, x + 54.0, y + 8.0);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 1.0);
    pango_cairo_show_layout(cr, tl);
    pango_font_description_free(tf);
    g_object_unref(tl);

    // Hi / Lo
    PangoLayout* hl = pango_cairo_create_layout(cr);
    PangoFontDescription* hf = pango_font_description_from_string("Sans 8.5");
    pango_layout_set_font_description(hl, hf);
    pango_layout_set_text(hl, hi_lo_str.c_str(), -1);
    PangoRectangle h_ink, h_log;
    pango_layout_get_pixel_extents(hl, &h_ink, &h_log);
    cairo_move_to(cr, x + w - 16.0 - h_ink.width, y + 12.0);
    cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.80);
    pango_cairo_show_layout(cr, hl);
    pango_font_description_free(hf);
    g_object_unref(hl);

    // Condition line
    PangoLayout* cl = pango_cairo_create_layout(cr);
    PangoFontDescription* cf = pango_font_description_from_string("Sans 8.5");
    pango_layout_set_font_description(cl, cf);
    pango_layout_set_text(cl, desc_str.c_str(), -1);
    cairo_move_to(cr, x + 54.0, y + 32.0);
    cairo_set_source_rgba(cr, 0.65, 0.63, 0.70, 0.90);
    pango_cairo_show_layout(cr, cl);
    pango_font_description_free(cf);
    g_object_unref(cl);

    // Location line
    PangoLayout* ll = pango_cairo_create_layout(cr);
    PangoFontDescription* lf = pango_font_description_from_string("Sans 8.0");
    pango_layout_set_font_description(ll, lf);
    pango_layout_set_text(ll, loc_str.c_str(), -1);
    cairo_move_to(cr, x + 54.0, y + 46.0);
    cairo_set_source_rgba(cr, 0.50, 0.48, 0.56, 0.85);
    pango_cairo_show_layout(cr, ll);
    pango_font_description_free(lf);
    g_object_unref(ll);

    // Hourly forecast row
    float forecast_y = y + 74.0f;
    float col_w = (w - 24.0f) / 4.0f;
    for (size_t i = 0; i < 4; ++i) {
        float fc_cx = x + 12.0f + i * col_w + col_w / 2.0f;

        // Time
        PangoLayout* ftime = pango_cairo_create_layout(cr);
        PangoFontDescription* ftf = pango_font_description_from_string("JetBrainsMono Nerd Font 7.5");
        pango_layout_set_font_description(ftime, ftf);
        pango_layout_set_text(ftime, fhours[i].time.c_str(), -1);
        PangoRectangle tink, tlog;
        pango_layout_get_pixel_extents(ftime, &tink, &tlog);
        cairo_move_to(cr, fc_cx - tink.width / 2.0 - tink.x, forecast_y);
        cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.80);
        pango_cairo_show_layout(cr, ftime);
        pango_font_description_free(ftf);
        g_object_unref(ftime);

        // Icon
        PangoLayout* fico = pango_cairo_create_layout(cr);
        PangoFontDescription* fif = pango_font_description_from_string("JetBrainsMono Nerd Font 10.0");
        pango_layout_set_font_description(fico, fif);
        pango_layout_set_text(fico, fhours[i].icon.c_str(), -1);
        PangoRectangle iink, ilog;
        pango_layout_get_pixel_extents(fico, &iink, &ilog);
        cairo_move_to(cr, fc_cx - iink.width / 2.0 - iink.x, forecast_y + 14.0);
        cairo_set_source_rgba(cr, 0.38, 0.54, 0.85, 0.90);
        pango_cairo_show_layout(cr, fico);
        pango_font_description_free(fif);
        g_object_unref(fico);

        // Temp
        PangoLayout* ftmp = pango_cairo_create_layout(cr);
        PangoFontDescription* ftpf = pango_font_description_from_string("Sans 8.0");
        pango_layout_set_font_description(ftmp, ftpf);
        pango_layout_set_text(ftmp, fhours[i].temp.c_str(), -1);
        PangoRectangle tmp_ink, tmp_log;
        pango_layout_get_pixel_extents(ftmp, &tmp_ink, &tmp_log);
        cairo_move_to(cr, fc_cx - tmp_ink.width / 2.0 - tmp_ink.x, forecast_y + 28.0);
        cairo_set_source_rgba(cr, 0.85, 0.83, 0.90, 0.90);
        pango_cairo_show_layout(cr, ftmp);
        pango_font_description_free(ftpf);
        g_object_unref(ftmp);

        // Pop %
        PangoLayout* fpop = pango_cairo_create_layout(cr);
        PangoFontDescription* fpf = pango_font_description_from_string("Sans 7.5");
        pango_layout_set_font_description(fpop, fpf);
        pango_layout_set_text(fpop, fhours[i].pop.c_str(), -1);
        PangoRectangle pop_ink, pop_log;
        pango_layout_get_pixel_extents(fpop, &pop_ink, &pop_log);
        cairo_move_to(cr, fc_cx - pop_ink.width / 2.0 - pop_ink.x, forecast_y + 41.0);
        cairo_set_source_rgba(cr, 0.35, 0.48, 0.75, 0.80);
        pango_cairo_show_layout(cr, fpop);
        pango_font_description_free(fpf);
        g_object_unref(fpop);
    }
}

void Renderer::draw_control_center(cairo_t* cr, float w, float h, const ControlCenterState& state,
                                  const MediaInfo& media, const NotificationItem& notif,
                                  bool mic_active, float anim_time) {
    (void)anim_time;
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    // Panel background (Obsidian Dark with subtle border)
    draw_rounded_rect(cr, 0.0, 0.0, w, h, 24.0);
    cairo_set_source_rgba(cr, 0.067, 0.071, 0.086, 0.98); // #111216
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.28, 0.26, 0.35, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // 1. Top Action Bar (y = 14..36)
    // Left quick buttons
    const char* left_icons[] = {"󰌾", "󰖔", "󰍃", "󰜉", "󰐥"};
    PangoLayout* top_l = pango_cairo_create_layout(cr);
    PangoFontDescription* top_f = pango_font_description_from_string("JetBrainsMono Nerd Font 11");
    pango_layout_set_font_description(top_l, top_f);

    for (int i = 0; i < 5; ++i) {
        float bx = 20.0f + i * 30.0f;
        pango_layout_set_text(top_l, left_icons[i], -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(top_l, &ink, &log);
        cairo_move_to(cr, bx - ink.width / 2.0 - ink.x, 22.0 - ink.height / 2.0 - ink.y);
        cairo_set_source_rgba(cr, 0.72, 0.70, 0.78, 0.85);
        pango_cairo_show_layout(cr, top_l);
    }

    // Right tab/utility buttons
    const char* right_icons[] = {"󰕰", "󰏖", "󰌹", "󰠮", "󰄲", "󰒓"};
    for (int i = 0; i < 6; ++i) {
        float bx = w - 190.0f + i * 30.0f;
        pango_layout_set_text(top_l, right_icons[i], -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(top_l, &ink, &log);
        cairo_move_to(cr, bx - ink.width / 2.0 - ink.x, 22.0 - ink.height / 2.0 - ink.y);
        cairo_set_source_rgba(cr, 0.72, 0.70, 0.78, 0.85);
        pango_cairo_show_layout(cr, top_l);
    }
    pango_font_description_free(top_f);
    g_object_unref(top_l);

    // ==========================================
    // COLUMN 1: LEFT (x = 16, w = 252)
    // ==========================================
    float col1_x = 16.0f;
    float col1_w = 252.0f;

    // Quick Settings Tiles (2x3 Grid)
    float tile_w = 122.0f;
    float tile_h = 46.0f;
    float tile_gap = 8.0f;

    struct TileData {
        const char* icon;
        std::string title;
        std::string subtitle;
        bool active;
        bool has_chevron;
    };

    std::string wifi_sub = state.wifi_enabled ? (state.wifi_ssid.empty() ? "Connected" : state.wifi_ssid) : "Off";
    std::string bt_sub = state.bluetooth_enabled ? "Connected" : "No devices";

    TileData tiles[6] = {
        {state.wifi_enabled ? "󰖩" : "󰖪", "Wi-Fi", wifi_sub, state.wifi_enabled, true},
        {state.bluetooth_enabled ? "󰂯" : "󰂲", "Bluetooth", bt_sub, state.bluetooth_enabled, false},
        {"󰓅", "Power", "balanced", false, false},
        {state.dnd_enabled ? "󰂛" : "󰂚", "Focus", state.dnd_enabled ? "Silent" : "Notifying", state.dnd_enabled, false},
        {"󰍬", "Microphone", mic_active ? "Live" : "Muted", mic_active, false},
        {"󰀝", "Airplane", "Radios on", false, false}
    };

    PangoLayout* tl_ico = pango_cairo_create_layout(cr);
    PangoFontDescription* tf_ico = pango_font_description_from_string("JetBrainsMono Nerd Font 11.5");
    pango_layout_set_font_description(tl_ico, tf_ico);

    PangoLayout* tl_title = pango_cairo_create_layout(cr);
    PangoFontDescription* tf_title = pango_font_description_from_string("Sans Bold 8.5");
    pango_layout_set_font_description(tl_title, tf_title);

    PangoLayout* tl_sub = pango_cairo_create_layout(cr);
    PangoFontDescription* tf_sub = pango_font_description_from_string("Sans 7.5");
    pango_layout_set_font_description(tl_sub, tf_sub);

    for (int i = 0; i < 6; ++i) {
        int r = i / 2;
        int c = i % 2;
        float tx = col1_x + c * (tile_w + tile_gap);
        float ty = 52.0f + r * (tile_h + 6.0f);

        draw_rounded_rect(cr, tx, ty, tile_w, tile_h, 11.0);
        if (tiles[i].active) {
            cairo_set_source_rgba(cr, 0.106, 0.133, 0.204, 0.95); // #1b2234
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.25, 0.35, 0.60, 0.40);
        } else {
            cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.95); // #16151b
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.18);
        }
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Circular badge for icon
        float badge_x = tx + 8.0f;
        float badge_y = ty + (tile_h - 28.0f) / 2.0f;
        draw_rounded_rect(cr, badge_x, badge_y, 28.0f, 28.0f, 14.0);
        if (tiles[i].active) {
            cairo_set_source_rgba(cr, 0.231, 0.349, 0.596, 0.95); // #3b5998
        } else {
            cairo_set_source_rgba(cr, 0.141, 0.133, 0.173, 0.95); // #24222c
        }
        cairo_fill(cr);

        // Icon inside badge
        pango_layout_set_text(tl_ico, tiles[i].icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(tl_ico, &ink, &log);
        cairo_move_to(cr, badge_x + 14.0 - ink.width / 2.0 - ink.x,
                          badge_y + 14.0 - ink.height / 2.0 - ink.y);
        if (tiles[i].active) {
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.85);
        }
        pango_cairo_show_layout(cr, tl_ico);

        // Title
        pango_layout_set_text(tl_title, tiles[i].title.c_str(), -1);
        cairo_move_to(cr, tx + 42.0, ty + 8.0);
        cairo_set_source_rgba(cr, 0.92, 0.90, 0.95, 0.95);
        pango_cairo_show_layout(cr, tl_title);

        // Subtitle
        pango_layout_set_text(tl_sub, tiles[i].subtitle.c_str(), -1);
        pango_layout_set_ellipsize(tl_sub, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(tl_sub, (tile_w - 50.0f) * PANGO_SCALE);
        cairo_move_to(cr, tx + 42.0, ty + 25.0);
        if (tiles[i].active) {
            cairo_set_source_rgba(cr, 0.38, 0.54, 0.85, 0.95);
        } else {
            cairo_set_source_rgba(cr, 0.48, 0.46, 0.54, 0.80);
        }
        pango_cairo_show_layout(cr, tl_sub);

        // Chevron
        if (tiles[i].has_chevron) {
            PangoLayout* ch = pango_cairo_create_layout(cr);
            PangoFontDescription* chf = pango_font_description_from_string("JetBrainsMono Nerd Font 8.5");
            pango_layout_set_font_description(ch, chf);
            pango_layout_set_text(ch, "󰅂", -1);
            cairo_move_to(cr, tx + tile_w - 14.0, ty + (tile_h - 10.0) / 2.0);
            cairo_set_source_rgba(cr, 0.48, 0.46, 0.54, 0.70);
            pango_cairo_show_layout(cr, ch);
            pango_font_description_free(chf);
            g_object_unref(ch);
        }
    }
    pango_font_description_free(tf_ico);
    pango_font_description_free(tf_title);
    pango_font_description_free(tf_sub);
    g_object_unref(tl_ico);
    g_object_unref(tl_title);
    g_object_unref(tl_sub);

    // Pagination Dots (y = 214)
    float dot_cx = col1_x + col1_w / 2.0f;
    cairo_arc(cr, dot_cx - 10.0, 214.0, 2.2, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.90, 0.90, 0.95, 0.95); // Active dot
    cairo_fill(cr);

    cairo_arc(cr, dot_cx, 214.0, 2.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.45, 0.42, 0.52, 0.60);
    cairo_fill(cr);

    PangoLayout* p_arr = pango_cairo_create_layout(cr);
    PangoFontDescription* paf = pango_font_description_from_string("JetBrainsMono Nerd Font 7.5");
    pango_layout_set_font_description(p_arr, paf);
    pango_layout_set_text(p_arr, "󰅂", -1);
    cairo_move_to(cr, dot_cx + 8.0, 209.0);
    cairo_set_source_rgba(cr, 0.45, 0.42, 0.52, 0.60);
    pango_cairo_show_layout(cr, p_arr);
    pango_font_description_free(paf);
    g_object_unref(p_arr);

    // Sliders (Volume: y = 226, Brightness: y = 270)
    auto draw_slider = [&](float y, const char* icon, float val, float press, float bounce) {
        val = std::clamp(val, 0.0f, 1.0f);
        float s_h = 36.0f;
        float h_press = s_h - (press * 2.0f);
        float y_press = y + (press * 1.0f);
        float r_press = h_press / 2.0f;

        // Track
        draw_rounded_rect(cr, col1_x, y_press, col1_w, h_press, r_press);
        cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.18);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Fill with bounce
        if (val > 0.005f || bounce != 0.0f) {
            float base_w = col1_w * val;
            float fill_w = std::clamp(base_w + bounce, h_press, col1_w + std::max(0.0f, bounce));
            if (fill_w > col1_w) fill_w = col1_w;
            if (fill_w > 0.0f) {
                draw_rounded_rect(cr, col1_x, y_press, fill_w, h_press, r_press);
                cairo_set_source_rgba(cr, 0.231, 0.349, 0.596, 0.95); // #3b5998
                cairo_fill(cr);
            }
        }

        // Icon inside slider on the left
        PangoLayout* si = pango_cairo_create_layout(cr);
        PangoFontDescription* sif = pango_font_description_from_string("JetBrainsMono Nerd Font 11.5");
        pango_layout_set_font_description(si, sif);
        pango_layout_set_text(si, icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(si, &ink, &log);
        cairo_move_to(cr, col1_x + 14.0 - ink.x, y_press + (h_press - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        pango_cairo_show_layout(cr, si);
        pango_font_description_free(sif);
        g_object_unref(si);

        // Percentage text on the right
        std::string pct_str = std::to_string(static_cast<int>(std::round(val * 100.0f))) + "%";
        PangoLayout* sp = pango_cairo_create_layout(cr);
        PangoFontDescription* spf = pango_font_description_from_string("Sans Bold 9.0");
        pango_layout_set_font_description(sp, spf);
        pango_layout_set_text(sp, pct_str.c_str(), -1);
        pango_layout_get_pixel_extents(sp, &ink, &log);
        cairo_move_to(cr, col1_x + col1_w - 14.0 - ink.width - ink.x,
                          y_press + (h_press - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 0.95);
        pango_cairo_show_layout(cr, sp);
        pango_font_description_free(spf);
        g_object_unref(sp);
    };

    draw_slider(226.0f, state.audio_muted ? "󰝟" : "󰕾", state.volume, state.volume_press, state.volume_bounce);
    draw_slider(270.0f, "󰃠", state.brightness, state.brightness_press, state.brightness_bounce);

    // Appearance / Wallpaper Card (y = 318..520, h = 202)
    float app_y = 318.0f;
    float app_h = 202.0f;

    cairo_save(cr);
    draw_rounded_rect(cr, col1_x, app_y, col1_w, app_h, 14.0);
    cairo_clip(cr);

    if (wallpaper_thumb_) {
        cairo_set_source_surface(cr, wallpaper_thumb_, col1_x, app_y);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0.12, 0.11, 0.16, 0.95);
        cairo_paint(cr);
    }

    // Bottom dark gradient overlay for text readability
    cairo_pattern_t* grad = cairo_pattern_create_linear(col1_x, app_y + 100.0, col1_x, app_y + app_h);
    cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.0, 0.0, 0.0, 0.0);
    cairo_pattern_add_color_stop_rgba(grad, 0.5, 0.05, 0.05, 0.07, 0.65);
    cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.04, 0.04, 0.06, 0.94);
    cairo_set_source(cr, grad);
    cairo_paint(cr);
    cairo_pattern_destroy(grad);

    // 4 Color Swatches (2x2 grid)
    float sw_x = col1_x + 12.0f;
    float sw_y = app_y + app_h - 38.0f;

    draw_rounded_rect(cr, sw_x, sw_y, 7.0, 7.0, 2.0);
    cairo_set_source_rgba(cr, 0.14, 0.13, 0.18, 0.95);
    cairo_fill(cr);

    draw_rounded_rect(cr, sw_x + 9.0, sw_y, 7.0, 7.0, 2.0);
    cairo_set_source_rgba(cr, 0.22, 0.21, 0.28, 0.95);
    cairo_fill(cr);

    draw_rounded_rect(cr, sw_x, sw_y + 9.0, 7.0, 7.0, 2.0);
    cairo_set_source_rgba(cr, 0.231, 0.349, 0.596, 0.95);
    cairo_fill(cr);

    draw_rounded_rect(cr, sw_x + 9.0, sw_y + 9.0, 7.0, 7.0, 2.0);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 0.95);
    cairo_fill(cr);

    // "Appearance" Title
    PangoLayout* apt = pango_cairo_create_layout(cr);
    PangoFontDescription* apt_f = pango_font_description_from_string("Sans Bold 9.5");
    pango_layout_set_font_description(apt, apt_f);
    pango_layout_set_text(apt, "Appearance", -1);
    cairo_move_to(cr, col1_x + 36.0, app_y + app_h - 40.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    pango_cairo_show_layout(cr, apt);
    pango_font_description_free(apt_f);
    g_object_unref(apt);

    // "Adaptive (Wallpaper)" Subtitle
    PangoLayout* aps = pango_cairo_create_layout(cr);
    PangoFontDescription* aps_f = pango_font_description_from_string("Sans 8.0");
    pango_layout_set_font_description(aps, aps_f);
    pango_layout_set_text(aps, "Adaptive (Wallpaper)", -1);
    cairo_move_to(cr, col1_x + 36.0, app_y + app_h - 24.0);
    cairo_set_source_rgba(cr, 0.75, 0.73, 0.80, 0.90);
    pango_cairo_show_layout(cr, aps);
    pango_font_description_free(aps_f);
    g_object_unref(aps);

    // Chevron on right
    PangoLayout* apc = pango_cairo_create_layout(cr);
    PangoFontDescription* apc_f = pango_font_description_from_string("JetBrainsMono Nerd Font 9.5");
    pango_layout_set_font_description(apc, apc_f);
    pango_layout_set_text(apc, "󰅂", -1);
    cairo_move_to(cr, col1_x + col1_w - 20.0, app_y + app_h - 32.0);
    cairo_set_source_rgba(cr, 0.85, 0.83, 0.90, 0.85);
    pango_cairo_show_layout(cr, apc);
    pango_font_description_free(apc_f);
    g_object_unref(apc);

    cairo_restore(cr);

    // ==========================================
    // COLUMN 2: CENTER (x = 284, w = 272)
    // ==========================================
    float col2_x = 284.0f;
    float col2_w = 272.0f;

    // 1. Media Player Card (y = 52..178, h = 126)
    float media_y = 52.0f;
    float media_h = 126.0f;

    draw_rounded_rect(cr, col2_x, media_y, col2_w, media_h, 14.0);
    cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.20);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    // Album art thumbnail
    float art_sz = 46.0f;
    float art_x = col2_x + 14.0f;
    float art_y = media_y + 14.0f;
    draw_rounded_rect(cr, art_x, art_y, art_sz, art_sz, 10.0);
    cairo_save(cr);
    cairo_clip(cr);
    if (art_surface_) {
        cairo_set_source_surface(cr, art_surface_, art_x, art_y);
        cairo_paint(cr);
    } else {
        cairo_set_source_rgba(cr, 0.14, 0.13, 0.18, 0.95);
        cairo_paint(cr);
        PangoLayout* no_art = pango_cairo_create_layout(cr);
        PangoFontDescription* naf = pango_font_description_from_string("JetBrainsMono Nerd Font 16");
        pango_layout_set_font_description(no_art, naf);
        pango_layout_set_text(no_art, "󰎆", -1);
        PangoRectangle n_ink, n_log;
        pango_layout_get_pixel_extents(no_art, &n_ink, &n_log);
        cairo_move_to(cr, art_x + (art_sz - n_ink.width) / 2.0 - n_ink.x,
                          art_y + (art_sz - n_ink.height) / 2.0 - n_ink.y);
        cairo_set_source_rgba(cr, 0.45, 0.42, 0.52, 0.90);
        pango_cairo_show_layout(cr, no_art);
        pango_font_description_free(naf);
        g_object_unref(no_art);
    }
    cairo_restore(cr);

    // Title & Artist
    std::string title_str = media.active && !media.title.empty() ? media.title : "Clair de Lune";
    std::string artist_str = media.active && !media.artist.empty() ? media.artist : "Claude Debussy";

    PangoLayout* mtl = pango_cairo_create_layout(cr);
    PangoFontDescription* mtf = pango_font_description_from_string("Sans Bold 10.5");
    pango_layout_set_font_description(mtl, mtf);
    pango_layout_set_text(mtl, title_str.c_str(), -1);
    pango_layout_set_ellipsize(mtl, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(mtl, (col2_w - 76.0f) * PANGO_SCALE);
    cairo_move_to(cr, art_x + art_sz + 12.0, art_y + 4.0);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 0.98);
    pango_cairo_show_layout(cr, mtl);
    pango_font_description_free(mtf);
    g_object_unref(mtl);

    PangoLayout* mal = pango_cairo_create_layout(cr);
    PangoFontDescription* maf = pango_font_description_from_string("Sans 8.5");
    pango_layout_set_font_description(mal, maf);
    pango_layout_set_text(mal, artist_str.c_str(), -1);
    pango_layout_set_ellipsize(mal, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(mal, (col2_w - 76.0f) * PANGO_SCALE);
    cairo_move_to(cr, art_x + art_sz + 12.0, art_y + 24.0);
    cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.85);
    pango_cairo_show_layout(cr, mal);
    pango_font_description_free(maf);
    g_object_unref(mal);

    // Slim Progress Bar
    float pbar_x = col2_x + 14.0f;
    float pbar_w = col2_w - 28.0f;
    float pbar_y = media_y + 72.0f;
    float pbar_h = 3.0f;
    float progress = (media.length_us > 0) ? std::clamp(static_cast<float>(media.position_us) / static_cast<float>(media.length_us), 0.0f, 1.0f) : 0.48f;

    draw_rounded_rect(cr, pbar_x, pbar_y, pbar_w, pbar_h, 1.5);
    cairo_set_source_rgba(cr, 0.18, 0.17, 0.22, 0.95);
    cairo_fill(cr);

    if (progress > 0.01f) {
        draw_rounded_rect(cr, pbar_x, pbar_y, pbar_w * progress, pbar_h, 1.5);
        cairo_set_source_rgba(cr, 0.231, 0.349, 0.596, 0.95);
        cairo_fill(cr);
    }

    // Media Controls & Timestamps
    float ctrl_y = media_y + 92.0f;

    int64_t cur_total_sec = media.position_us / 1000000;
    int cur_m = static_cast<int>(cur_total_sec / 60);
    int cur_s = static_cast<int>(cur_total_sec % 60);
    if (!media.active) { cur_m = 2; cur_s = 24; }
    std::ostringstream cur_oss;
    cur_oss << cur_m << ":" << std::setfill('0') << std::setw(2) << cur_s;

    PangoLayout* ctl = pango_cairo_create_layout(cr);
    PangoFontDescription* cf = pango_font_description_from_string("JetBrainsMono Nerd Font 8.0");
    pango_layout_set_font_description(ctl, cf);
    pango_layout_set_text(ctl, cur_oss.str().c_str(), -1);
    cairo_move_to(cr, pbar_x, ctrl_y + 2.0);
    cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.80);
    pango_cairo_show_layout(cr, ctl);
    g_object_unref(ctl);

    int64_t tot_total_sec = media.length_us / 1000000;
    int tot_m = static_cast<int>(tot_total_sec / 60);
    int tot_s = static_cast<int>(tot_total_sec % 60);
    if (!media.active || media.length_us <= 0) { tot_m = 5; tot_s = 0; }
    std::ostringstream tot_oss;
    tot_oss << tot_m << ":" << std::setfill('0') << std::setw(2) << tot_s;

    PangoLayout* ttl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(ttl, cf);
    pango_layout_set_text(ttl, tot_oss.str().c_str(), -1);
    PangoRectangle tink, tlog;
    pango_layout_get_pixel_extents(ttl, &tink, &tlog);
    cairo_move_to(cr, pbar_x + pbar_w - tink.width - tink.x, ctrl_y + 2.0);
    cairo_set_source_rgba(cr, 0.55, 0.53, 0.60, 0.80);
    pango_cairo_show_layout(cr, ttl);
    g_object_unref(ttl);
    pango_font_description_free(cf);

    PangoLayout* mbtn = pango_cairo_create_layout(cr);
    PangoFontDescription* mbf = pango_font_description_from_string("JetBrainsMono Nerd Font 13.0");
    pango_layout_set_font_description(mbtn, mbf);

    float mid_cx = col2_x + col2_w / 2.0f;

    pango_layout_set_text(mbtn, "󰒮", -1);
    PangoRectangle pink, plog;
    pango_layout_get_pixel_extents(mbtn, &pink, &plog);
    cairo_move_to(cr, mid_cx - 36.0 - pink.width / 2.0 - pink.x, ctrl_y);
    cairo_set_source_rgba(cr, 0.85, 0.83, 0.90, 0.90);
    pango_cairo_show_layout(cr, mbtn);

    pango_layout_set_text(mbtn, media.playing ? "󰏤" : "󰐊", -1);
    pango_layout_get_pixel_extents(mbtn, &pink, &plog);
    cairo_move_to(cr, mid_cx - pink.width / 2.0 - pink.x, ctrl_y);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 1.0);
    pango_cairo_show_layout(cr, mbtn);

    pango_layout_set_text(mbtn, "󰒭", -1);
    pango_layout_get_pixel_extents(mbtn, &pink, &plog);
    cairo_move_to(cr, mid_cx + 36.0 - pink.width / 2.0 - pink.x, ctrl_y);
    cairo_set_source_rgba(cr, 0.85, 0.83, 0.90, 0.90);
    pango_cairo_show_layout(cr, mbtn);

    pango_font_description_free(mbf);
    g_object_unref(mbtn);

    // 2. Weather Card (y = 186..310, h = 124)
    draw_weather_card(cr, col2_x, 186.0f, col2_w, 124.0f);

    // 3. Monthly Calendar Card (y = 318..520, h = 202)
    draw_calendar(cr, col2_x, 318.0f, col2_w, 202.0f, tm_info);

    // ==========================================
    // COLUMN 3: RIGHT (x = 572, w = 242)
    // ==========================================
    float col3_x = 572.0f;
    float col3_w = 242.0f;
    float col3_y = 52.0f;
    float col3_h = 468.0f;

    draw_rounded_rect(cr, col3_x, col3_y, col3_w, col3_h, 14.0);
    cairo_set_source_rgba(cr, 0.086, 0.082, 0.106, 0.96);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.20);
    cairo_set_line_width(cr, 0.8);
    cairo_stroke(cr);

    // Header: "Notifications"
    PangoLayout* nhl = pango_cairo_create_layout(cr);
    PangoFontDescription* nhf = pango_font_description_from_string("Sans Bold 11");
    pango_layout_set_font_description(nhl, nhf);
    pango_layout_set_text(nhl, "Notifications", -1);
    cairo_move_to(cr, col3_x + 16.0, col3_y + 16.0);
    cairo_set_source_rgba(cr, 0.95, 0.95, 0.98, 0.98);
    pango_cairo_show_layout(cr, nhl);
    pango_font_description_free(nhf);
    g_object_unref(nhl);

    // Content: Active card or "Nothing new"
    if (!notif.summary.empty() || !notif.body.empty()) {
        float card_x = col3_x + 12.0f;
        float card_y = col3_y + 48.0f;
        float card_w = col3_w - 24.0f;
        float card_h = 80.0f;

        draw_rounded_rect(cr, card_x, card_y, card_w, card_h, 10.0);
        cairo_set_source_rgba(cr, 0.12, 0.11, 0.15, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.35, 0.32, 0.42, 0.25);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        PangoLayout* anl = pango_cairo_create_layout(cr);
        PangoFontDescription* anf = pango_font_description_from_string("Sans Bold 8.5");
        pango_layout_set_font_description(anl, anf);
        pango_layout_set_text(anl, notif.app_name.empty() ? "System" : notif.app_name.c_str(), -1);
        cairo_move_to(cr, card_x + 10.0, card_y + 8.0);
        cairo_set_source_rgba(cr, 0.45, 0.65, 0.95, 0.95);
        pango_cairo_show_layout(cr, anl);
        pango_font_description_free(anf);
        g_object_unref(anl);

        PangoLayout* sml = pango_cairo_create_layout(cr);
        PangoFontDescription* smf = pango_font_description_from_string("Sans SemiBold 9.0");
        pango_layout_set_font_description(sml, smf);
        pango_layout_set_text(sml, notif.summary.c_str(), -1);
        pango_layout_set_ellipsize(sml, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(sml, (card_w - 20.0f) * PANGO_SCALE);
        cairo_move_to(cr, card_x + 10.0, card_y + 24.0);
        cairo_set_source_rgba(cr, 0.92, 0.90, 0.95, 0.95);
        pango_cairo_show_layout(cr, sml);
        pango_font_description_free(smf);
        g_object_unref(sml);

        PangoLayout* bdl = pango_cairo_create_layout(cr);
        PangoFontDescription* bdf = pango_font_description_from_string("Sans 8.0");
        pango_layout_set_font_description(bdl, bdf);
        pango_layout_set_text(bdl, notif.body.c_str(), -1);
        pango_layout_set_ellipsize(bdl, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(bdl, (card_w - 20.0f) * PANGO_SCALE);
        cairo_move_to(cr, card_x + 10.0, card_y + 44.0);
        cairo_set_source_rgba(cr, 0.65, 0.63, 0.70, 0.85);
        pango_cairo_show_layout(cr, bdl);
        pango_font_description_free(bdf);
        g_object_unref(bdl);
    } else {
        PangoLayout* empty_l = pango_cairo_create_layout(cr);
        PangoFontDescription* ef = pango_font_description_from_string("Sans 9.5");
        pango_layout_set_font_description(empty_l, ef);
        pango_layout_set_text(empty_l, "Nothing new", -1);
        PangoRectangle eink, elog;
        pango_layout_get_pixel_extents(empty_l, &eink, &elog);
        cairo_move_to(cr, col3_x + (col3_w - eink.width) / 2.0 - eink.x,
                          col3_y + (col3_h - eink.height) / 2.0 - eink.y);
        cairo_set_source_rgba(cr, 0.50, 0.48, 0.56, 0.65);
        pango_cairo_show_layout(cr, empty_l);
        pango_font_description_free(ef);
        g_object_unref(empty_l);
    }
}

void Renderer::draw_hud_osd(cairo_t* cr, float w, float h, const OsdState& osd) {
    const char* icon_str = "󰕾";
    double r_color = 0.816, g_color = 0.737, b_color = 1.0; // M3 Lavender

    if (osd.type == OsdType::Volume) {
        if (osd.muted) {
            icon_str = "󰝟";
            r_color = 0.98; g_color = 0.58; b_color = 0.52; // Coral
        } else if (osd.value < 0.01f) {
            icon_str = "󰝟";
            r_color = 0.584; g_color = 0.557; b_color = 0.612;
        } else if (osd.value < 0.33f) {
            icon_str = "󰕿";
        } else if (osd.value < 0.66f) {
            icon_str = "󰖀";
        } else {
            icon_str = "󰕾";
        }
    } else if (osd.type == OsdType::Brightness) {
        r_color = 1.0; g_color = 0.824; b_color = 0.490; // M3 Amber
        if (osd.value < 0.33f) {
            icon_str = "󰃞";
        } else if (osd.value < 0.66f) {
            icon_str = "󰃝";
        } else {
            icon_str = "󰃠";
        }
    } else { // Battery
        if (osd.is_charging) {
            icon_str = "󰂄";
            r_color = 0.502; g_color = 0.871; b_color = 0.773; // M3 Mint
        } else if (osd.value < 0.2f) {
            icon_str = "󰂃";
            r_color = 0.98; g_color = 0.58; b_color = 0.52; // Coral
        } else {
            icon_str = "󰁹";
            r_color = 0.502; g_color = 0.871; b_color = 0.773;
        }
    }

    // 1. Icon
    PangoLayout* icon_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* icon_font = pango_font_description_from_string("JetBrainsMono Nerd Font 12");
    pango_layout_set_font_description(icon_layout, icon_font);
    pango_layout_set_text(icon_layout, icon_str, -1);

    PangoRectangle icon_ink, icon_log;
    pango_layout_get_pixel_extents(icon_layout, &icon_ink, &icon_log);
    double icon_y = (h - icon_ink.height) / 2.0 - icon_ink.y;

    cairo_move_to(cr, 13.0 - icon_ink.x, icon_y);
    cairo_set_source_rgba(cr, r_color, g_color, b_color, 0.98);
    pango_cairo_show_layout(cr, icon_layout);
    pango_font_description_free(icon_font);
    g_object_unref(icon_layout);

    // 2. Value Text
    std::string val_str;
    if (osd.type == OsdType::Volume && osd.muted) {
        val_str = "MUTE";
    } else if (osd.type == OsdType::Battery && osd.is_charging) {
        val_str = std::to_string(static_cast<int>(std::round(osd.value * 100.0f))) + "% ⚡";
    } else {
        int pct = static_cast<int>(std::round(osd.value * 100.0f));
        val_str = std::to_string(pct) + "%";
    }

    PangoLayout* val_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* val_font = pango_font_description_from_string("Sans Bold 9.5");
    pango_layout_set_font_description(val_layout, val_font);
    pango_layout_set_text(val_layout, val_str.c_str(), -1);

    PangoRectangle val_ink, val_log;
    pango_layout_get_pixel_extents(val_layout, &val_ink, &val_log);
    double val_y = (h - val_ink.height) / 2.0 - val_ink.y;
    double val_x = w - 12.0 - val_ink.width - val_ink.x;

    cairo_move_to(cr, val_x, val_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
    pango_cairo_show_layout(cr, val_layout);
    pango_font_description_free(val_font);
    g_object_unref(val_layout);

    // 3. Fluid Capsule Progress Bar
    float bar_x = 36.0f;
    float bar_w = static_cast<float>(val_x) - bar_x - 10.0f;
    if (bar_w < 20.0f) bar_w = 20.0f;
    float bar_h = 7.0f;
    float bar_y = (h - bar_h) / 2.0f;

    draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 3.5);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.90);
    cairo_fill(cr);

    float fill_ratio = std::clamp(osd.value, 0.0f, 1.0f);
    if (fill_ratio > 0.01f && (!osd.muted || osd.type != OsdType::Volume)) {
        float fill_w = std::max(7.0f, bar_w * fill_ratio);
        draw_rounded_rect(cr, bar_x, bar_y, fill_w, bar_h, 3.5);
        cairo_set_source_rgba(cr, r_color, g_color, b_color, 0.95);
        cairo_fill(cr);
    }
}

void Renderer::draw_notification(cairo_t* cr, float w, float h, const NotificationItem& notif) {
    (void)h;
    // 1. Notification Icon / Badge on Left
    float badge_x = 16.0f;
    float badge_y = 15.0f;
    float badge_size = 38.0f;

    draw_rounded_rect(cr, badge_x, badge_y, badge_size, badge_size, 10.0);
    cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.95);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Bell / Message icon
    PangoLayout* bell_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* bell_font = pango_font_description_from_string("JetBrainsMono Nerd Font 15");
    pango_layout_set_font_description(bell_layout, bell_font);
    pango_layout_set_text(bell_layout, "󰂚", -1);

    PangoRectangle bell_ink, bell_log;
    pango_layout_get_pixel_extents(bell_layout, &bell_ink, &bell_log);
    cairo_move_to(cr, badge_x + (badge_size - bell_ink.width) / 2.0 - bell_ink.x,
                      badge_y + (badge_size - bell_ink.height) / 2.0 - bell_ink.y);
    cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.95);
    pango_cairo_show_layout(cr, bell_layout);
    pango_font_description_free(bell_font);
    g_object_unref(bell_layout);

    // 2. Summary & App Name
    float text_x = badge_x + badge_size + 12.0f;
    float text_w = w - text_x - 14.0f;

    PangoLayout* title_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* title_font = pango_font_description_from_string("Sans Bold 10.5");
    pango_layout_set_font_description(title_layout, title_font);
    pango_layout_set_text(title_layout, notif.summary.empty() ? "Notificación" : notif.summary.c_str(), -1);
    pango_layout_set_ellipsize(title_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(title_layout, text_w * PANGO_SCALE);

    cairo_move_to(cr, text_x, badge_y + 2.0);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 1.0);
    pango_cairo_show_layout(cr, title_layout);
    pango_font_description_free(title_font);
    g_object_unref(title_layout);

    // 3. Body text
    std::string body_text = notif.body;
    if (body_text.empty() && !notif.app_name.empty()) {
        body_text = notif.app_name;
    }

    PangoLayout* body_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* body_font = pango_font_description_from_string("Sans 9.5");
    pango_layout_set_font_description(body_layout, body_font);
    pango_layout_set_text(body_layout, body_text.c_str(), -1);
    pango_layout_set_ellipsize(body_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(body_layout, text_w * PANGO_SCALE);

    cairo_move_to(cr, text_x, badge_y + 21.0);
    cairo_set_source_rgba(cr, 0.792, 0.769, 0.816, 0.85);
    pango_cairo_show_layout(cr, body_layout);
    pango_font_description_free(body_font);
    g_object_unref(body_layout);
}

} // namespace island
