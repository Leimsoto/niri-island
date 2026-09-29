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
    if (media.active && media.playing && !media.title.empty()) {
        update_art(media.art_url);
    } else {
        update_art("");
    }

    // 1. Calculate symmetrical side wings so the main island is perfectly centered horizontally
    float left_wing = (ws_bubble_w > 0.0f) ? (ws_bubble_w + 8.0f) : 0.0f;
    float right_wing = 0.0f;
    float wing = std::max(left_wing, right_wing);

    float main_x = wing;
    float main_w = std::max(20.0f, width - 2.0f * wing);

    if (ws_bubble_w > 0.0f) {
        float ws_x = wing - 8.0f - ws_bubble_w;
        draw_workspace_bubble(cr, ws_x, 0.0f, ws_bubble_w, 32.0f, workspaces, active_ws_idx);
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
            draw_control_center(cr, main_w, height, cc_state, anim_time);
            break;
        case IslandMode::HudOSD:
            draw_hud_osd(cr, main_w, height, osd);
            break;
        case IslandMode::Notification:
            draw_notification(cr, main_w, height, notif);
            break;
    }

    cairo_restore(cr);
    (void)mic_active;
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
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.92); // M3 Primary Lavender
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

void Renderer::draw_control_center(cairo_t* cr, float w, float h, const ControlCenterState& state, float) {
    (void)h;
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);

    char time_str[16];
    char date_str[48];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    strftime(date_str, sizeof(date_str), "%A, %d de %B", tm_info);

    // 1. Header (y = 16..46): Time, Date on left, Quick Actions on right
    PangoLayout* clock_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* clock_font = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 16");
    pango_layout_set_font_description(clock_layout, clock_font);
    pango_layout_set_text(clock_layout, time_str, -1);
    cairo_move_to(cr, 20.0, 14.0);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 1.0); // M3 On-Surface
    pango_cairo_show_layout(cr, clock_layout);
    pango_font_description_free(clock_font);
    g_object_unref(clock_layout);

    PangoLayout* date_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* date_font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(date_layout, date_font);
    pango_layout_set_text(date_layout, date_str, -1);
    cairo_move_to(cr, 86.0, 18.0);
    cairo_set_source_rgba(cr, 0.792, 0.769, 0.816, 0.85); // M3 Muted
    pango_cairo_show_layout(cr, date_layout);
    pango_font_description_free(date_font);
    g_object_unref(date_layout);

    // Header Quick Action Buttons (26x26, 8px gap)
    auto draw_header_btn = [&](float x, const char* icon, bool highlight) {
        float btn_sz = 26.0f;
        draw_rounded_rect(cr, x, 14.0f, btn_sz, btn_sz, 13.0);
        if (highlight) {
            cairo_set_source_rgba(cr, 0.310, 0.216, 0.545, 0.90); // M3 Primary Container
        } else {
            cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.90); // Surface Container High
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.30);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        PangoLayout* ico = pango_cairo_create_layout(cr);
        PangoFontDescription* f = pango_font_description_from_string("JetBrainsMono Nerd Font 11");
        pango_layout_set_font_description(ico, f);
        pango_layout_set_text(ico, icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(ico, &ink, &log);
        cairo_move_to(cr, x + (btn_sz - ink.width) / 2.0 - ink.x,
                          14.0 + (btn_sz - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, highlight ? 0.918 : 0.80, highlight ? 0.867 : 0.78, 1.0, 0.95);
        pango_cairo_show_layout(cr, ico);
        pango_font_description_free(f);
        g_object_unref(ico);
    };

    // Close button (far right: w - 42)
    draw_header_btn(w - 42.0f, "󰅖", false);
    // Lock button (w - 76)
    draw_header_btn(w - 76.0f, "󰌾", false);
    // Screenshot button (w - 110)
    draw_header_btn(w - 110.0f, "󰄀", false);
    // Media toggle button (if media active: w - 144)
    if (state.media_active) {
        draw_header_btn(w - 144.0f, "󰎆", true);
    }

    // 2. System Stats Bar (y = 52..80, h = 28): CPU, RAM, Battery chips
    float stats_y = 52.0f;
    float stat_chip_gap = 8.0f;
    float stat_chip_w = (w - 40.0f - stat_chip_gap * 2.0f) / 3.0f;
    float stat_chip_h = 28.0f;

    auto draw_stat_chip = [&](float x, const char* icon, const std::string& val_str, double r, double g, double b) {
        draw_rounded_rect(cr, x, stats_y, stat_chip_w, stat_chip_h, 8.0);
        cairo_set_source_rgba(cr, 0.125, 0.106, 0.165, 0.90); // Surface Container
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.22);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Icon
        PangoLayout* il = pango_cairo_create_layout(cr);
        PangoFontDescription* ifont = pango_font_description_from_string("JetBrainsMono Nerd Font 10");
        pango_layout_set_font_description(il, ifont);
        pango_layout_set_text(il, icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(il, &ink, &log);
        cairo_move_to(cr, x + 8.0 - ink.x, stats_y + (stat_chip_h - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, r, g, b, 0.95);
        pango_cairo_show_layout(cr, il);
        pango_font_description_free(ifont);
        g_object_unref(il);

        // Value text
        PangoLayout* vl = pango_cairo_create_layout(cr);
        PangoFontDescription* vfont = pango_font_description_from_string("JetBrainsMono Nerd Font SemiBold 9.0");
        pango_layout_set_font_description(vl, vfont);
        pango_layout_set_text(vl, val_str.c_str(), -1);
        PangoRectangle vink, vlog;
        pango_layout_get_pixel_extents(vl, &vink, &vlog);
        cairo_move_to(cr, x + 24.0, stats_y + (stat_chip_h - vink.height) / 2.0 - vink.y);
        cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.92);
        pango_cairo_show_layout(cr, vl);
        pango_font_description_free(vfont);
        g_object_unref(vl);
    };

    // CPU Stat (Muted gray icon like Wi-Fi button)
    std::string cpu_str = "CPU " + std::to_string(static_cast<int>(std::round(state.stats.cpu_percent))) + "%";
    draw_stat_chip(20.0f, "󰍛", cpu_str, 0.584, 0.557, 0.612);

    // RAM Stat (Muted gray icon like Wi-Fi button)
    std::string ram_str = "RAM " + std::to_string(static_cast<int>(std::round(state.stats.ram_percent))) + "%";
    draw_stat_chip(20.0f + stat_chip_w + stat_chip_gap, "󰘚", ram_str, 0.584, 0.557, 0.612);

    // Battery Stat (Muted gray icon like Wi-Fi button)
    std::string bat_str = std::to_string(state.battery_percent) + "%";
    draw_stat_chip(20.0f + (stat_chip_w + stat_chip_gap) * 2.0f,
                   state.battery_charging ? "󰂄" : "󰁹", bat_str, 0.584, 0.557, 0.612);

    // 3. Tonal Toggle Chips (2x2 Grid) (y = 90..176)
    float chip_w = (w - 40.0f - 12.0f) / 2.0f;
    float chip_h = 38.0f;
    float col1_x = 20.0f;
    float col2_x = 20.0f + chip_w + 12.0f;
    float row1_y = 90.0f;
    float row2_y = 136.0f;

    auto draw_chip = [&](float x, float y, const char* icon, const std::string& label, bool active, bool alert = false) {
        draw_rounded_rect(cr, x, y, chip_w, chip_h, 10.0);
        if (alert && active) {
            cairo_set_source_rgba(cr, 0.38, 0.14, 0.16, 0.92); // M3 Error Container
        } else if (active) {
            cairo_set_source_rgba(cr, 0.310, 0.216, 0.545, 0.92); // M3 Primary Container
        } else {
            cairo_set_source_rgba(cr, 0.125, 0.106, 0.165, 0.88); // M3 Surface Container
        }
        cairo_fill_preserve(cr);

        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, active ? 0.40 : 0.22);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Icon
        PangoLayout* i_layout = pango_cairo_create_layout(cr);
        PangoFontDescription* i_font = pango_font_description_from_string("JetBrainsMono Nerd Font 12");
        pango_layout_set_font_description(i_layout, i_font);
        pango_layout_set_text(i_layout, icon, -1);
        PangoRectangle i_ink, i_log;
        pango_layout_get_pixel_extents(i_layout, &i_ink, &i_log);
        cairo_move_to(cr, x + 12.0 - i_ink.x, y + (chip_h - i_ink.height) / 2.0 - i_ink.y);
        if (alert && active) {
            cairo_set_source_rgba(cr, 0.98, 0.65, 0.60, 1.0);
        } else if (active) {
            cairo_set_source_rgba(cr, 0.90, 0.85, 1.0, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.85);
        }
        pango_cairo_show_layout(cr, i_layout);
        pango_font_description_free(i_font);
        g_object_unref(i_layout);

        // Label
        PangoLayout* l_layout = pango_cairo_create_layout(cr);
        PangoFontDescription* l_font = pango_font_description_from_string("Sans SemiBold 9.0");
        pango_layout_set_font_description(l_layout, l_font);
        pango_layout_set_text(l_layout, label.c_str(), -1);
        pango_layout_set_ellipsize(l_layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(l_layout, (chip_w - 44.0f) * PANGO_SCALE);

        PangoRectangle l_ink, l_log;
        pango_layout_get_pixel_extents(l_layout, &l_ink, &l_log);
        cairo_move_to(cr, x + 34.0, y + (chip_h - l_ink.height) / 2.0 - l_ink.y);
        cairo_set_source_rgba(cr, active ? 0.95 : 0.80, active ? 0.95 : 0.80, active ? 1.0 : 0.85, 0.95);
        pango_cairo_show_layout(cr, l_layout);
        pango_font_description_free(l_font);
        g_object_unref(l_layout);
    };

    // Wi-Fi
    draw_chip(col1_x, row1_y, state.wifi_enabled ? "󰖩" : "󰖪",
              state.wifi_enabled ? (state.wifi_ssid.empty() ? "Wi-Fi" : state.wifi_ssid) : "Desactivado",
              state.wifi_enabled);

    // Bluetooth
    draw_chip(col2_x, row1_y, state.bluetooth_enabled ? "󰂯" : "󰂲",
              state.bluetooth_enabled ? "Bluetooth" : "Desactivado",
              state.bluetooth_enabled);

    // Audio Mute
    draw_chip(col1_x, row2_y, state.audio_muted ? "󰝟" : "󰕾",
              state.audio_muted ? "Silenciado" : "Sonido Activo",
              state.audio_muted, true);

    // DND
    draw_chip(col2_x, row2_y, state.dnd_enabled ? "󰂛" : "󰂚",
              state.dnd_enabled ? "No Molestar" : "Notificaciones",
              state.dnd_enabled);

    // 4. Material 3 LevelRows (Thick Sliders with Tactile Depth & Bounce) (y = 188..270)
    float s_x = 20.0f;
    float s_w = w - 40.0f;
    float s_h = 34.0f;

    auto draw_slider = [&](float y, const char* icon, float val, double r_fill, double g_fill, double b_fill, float press, float bounce) {
        val = std::clamp(val, 0.0f, 1.0f);
        float h_press = s_h - (press * 3.0f);
        float y_press = y + (press * 1.5f);
        float r_press = h_press / 2.0f;

        // Track with press depth
        draw_rounded_rect(cr, s_x, y_press, s_w, h_press, r_press);
        cairo_set_source_rgba(cr, 0.125 - press * 0.03, 0.106 - press * 0.03, 0.165 - press * 0.03, 0.92);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.20 + press * 0.12);
        cairo_set_line_width(cr, 0.8 + press * 0.4);
        cairo_stroke(cr);

        // Active fill with bounce elasticity
        if (val > 0.005f || bounce != 0.0f) {
            float base_w = s_w * val;
            float fill_w = std::clamp(base_w + bounce, h_press, s_w + std::max(0.0f, bounce));
            if (fill_w > s_w) fill_w = s_w;
            if (val <= 0.01f && bounce <= 0.0f) fill_w = 0.0f;
            if (fill_w > 0.0f) {
                draw_rounded_rect(cr, s_x, y_press, fill_w, h_press, r_press);
                cairo_set_source_rgba(cr, r_fill, g_fill, b_fill, 0.95);
                cairo_fill(cr);
            }
        }

        // Icon inside slider on the left
        PangoLayout* s_icon = pango_cairo_create_layout(cr);
        PangoFontDescription* si_font = pango_font_description_from_string("JetBrainsMono Nerd Font 12");
        pango_layout_set_font_description(s_icon, si_font);
        pango_layout_set_text(s_icon, icon, -1);
        PangoRectangle si_ink, si_log;
        pango_layout_get_pixel_extents(s_icon, &si_ink, &si_log);
        cairo_move_to(cr, s_x + 14.0 - si_ink.x, y_press + (h_press - si_ink.height) / 2.0 - si_ink.y);
        if (val > 0.16f) {
            cairo_set_source_rgba(cr, 0.12, 0.08, 0.18, 1.0); // Contrast on active bar
        } else {
            cairo_set_source_rgba(cr, r_fill, g_fill, b_fill, 0.95);
        }
        pango_cairo_show_layout(cr, s_icon);
        pango_font_description_free(si_font);
        g_object_unref(s_icon);

        // Percentage text inside slider on the right
        std::string pct_str = std::to_string(static_cast<int>(std::round(val * 100.0f))) + "%";
        PangoLayout* s_pct = pango_cairo_create_layout(cr);
        PangoFontDescription* sp_font = pango_font_description_from_string("Sans Bold 9.5");
        pango_layout_set_font_description(s_pct, sp_font);
        pango_layout_set_text(s_pct, pct_str.c_str(), -1);
        PangoRectangle sp_ink, sp_log;
        pango_layout_get_pixel_extents(s_pct, &sp_ink, &sp_log);
        cairo_move_to(cr, s_x + s_w - 14.0 - sp_ink.width - sp_ink.x, y_press + (h_press - sp_ink.height) / 2.0 - sp_ink.y);
        cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
        pango_cairo_show_layout(cr, s_pct);
        pango_font_description_free(sp_font);
        g_object_unref(s_pct);
    };

    // Volume Slider (with press depth and bounce)
    draw_slider(188.0f, state.audio_muted ? "󰝟" : "󰕾", state.volume, 0.816, 0.737, 1.0, state.volume_press, state.volume_bounce);

    // Brightness Slider (with press depth and bounce)
    draw_slider(232.0f, "󰃠", state.brightness, 0.92, 0.78, 0.45, state.brightness_press, state.brightness_bounce);

    // 5. Subtle System Identifier Footer (y = 284..310)
    float foot_y = 286.0f;
    PangoLayout* foot_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* foot_font = pango_font_description_from_string("JetBrainsMono Nerd Font 9.0");
    pango_layout_set_font_description(foot_layout, foot_font);
    pango_layout_set_text(foot_layout, "leimsoto@debian  •  niri", -1);
    PangoRectangle fi_ink, fi_log;
    pango_layout_get_pixel_extents(foot_layout, &fi_ink, &fi_log);
    cairo_move_to(cr, (w - fi_ink.width) / 2.0 - fi_ink.x, foot_y);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.70); // Subtle M3 Outline
    pango_cairo_show_layout(cr, foot_layout);
    pango_font_description_free(foot_font);
    g_object_unref(foot_layout);
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
