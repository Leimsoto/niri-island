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

cairo_surface_t* Renderer::load_image_scaled(const std::string& path_or_uri, int target_w, int target_h) {
    if (path_or_uri.empty()) return nullptr;

    std::string path = path_or_uri;
    if (path.rfind("file://", 0) == 0) {
        char* local = g_filename_from_uri(path.c_str(), nullptr, nullptr);
        if (local) {
            path = local;
            g_free(local);
        } else {
            path = path.substr(7);
        }
    }

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
    if (art_url == current_art_url_) return;

    current_art_url_ = art_url;
    if (art_surface_) {
        cairo_surface_destroy(art_surface_);
        art_surface_ = nullptr;
    }
    if (mini_art_surface_) {
        cairo_surface_destroy(mini_art_surface_);
        mini_art_surface_ = nullptr;
    }

    if (!art_url.empty()) {
        art_surface_ = load_image_scaled(art_url, 48, 48);
        mini_art_surface_ = load_image_scaled(art_url, 22, 22);
    }
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

void Renderer::draw_glass_sheen(cairo_t* cr, float w, float h, double r) {
    cairo_save(cr);
    draw_rounded_rect(cr, 1.0, 1.0, w - 2.0, h - 2.0, r);
    cairo_clip(cr);

    float sheen_h = std::min(h * 0.45f, 40.0f);
    cairo_pattern_t* pat = cairo_pattern_create_linear(0, 0, 0, sheen_h);
    cairo_pattern_add_color_stop_rgba(pat, 0.0, 1.0, 1.0, 1.0, 0.12);
    cairo_pattern_add_color_stop_rgba(pat, 0.5, 1.0, 1.0, 1.0, 0.04);
    cairo_pattern_add_color_stop_rgba(pat, 1.0, 1.0, 1.0, 1.0, 0.0);
    cairo_set_source(cr, pat);
    cairo_rectangle(cr, 0, 0, w, sheen_h);
    cairo_fill(cr);
    cairo_pattern_destroy(pat);

    cairo_restore(cr);
}

void Renderer::draw_lit_rim(cairo_t* cr, float w, float h, double r) {
    cairo_save(cr);
    draw_rounded_rect(cr, 1.0, 1.0, w - 2.0, h - 2.0, r);
    cairo_pattern_t* rim_pat = cairo_pattern_create_linear(0, 0, 0, h);
    cairo_pattern_add_color_stop_rgba(rim_pat, 0.0, 0.90, 0.85, 1.0, 0.35);
    cairo_pattern_add_color_stop_rgba(rim_pat, 0.35, 0.60, 0.50, 0.75, 0.18);
    cairo_pattern_add_color_stop_rgba(rim_pat, 1.0, 0.30, 0.25, 0.40, 0.12);
    cairo_set_source(cr, rim_pat);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    cairo_pattern_destroy(rim_pat);
    cairo_restore(cr);
}

void Renderer::draw(cairo_t* cr, float width, float height, IslandMode mode,
                    const MediaInfo& media, const OsdState& osd,
                    const NotificationItem& notif, const ControlCenterState& cc_state,
                    bool mic_active, float anim_time) {
    update_art(media.art_url);

    double radius = std::min((double)height / 2.0, 22.0);

    // 1. Draw Island Body (Material 3 Dark Surface #131019)
    draw_rounded_rect(cr, 1.0, 1.0, width - 2.0, height - 2.0, radius);
    cairo_set_source_rgba(cr, 0.075, 0.063, 0.098, 0.96);
    cairo_fill(cr);

    // 2. Impasto Glass Sheen (Top-down physical lighting)
    draw_glass_sheen(cr, width, height, radius);

    // 3. Impasto Lit Rim (Edge highlight)
    draw_lit_rim(cr, width, height, radius);

    // Clip content to rounded pill
    cairo_save(cr);
    draw_rounded_rect(cr, 2.0, 2.0, width - 4.0, height - 4.0, radius - 1.0);
    cairo_clip(cr);

    // 4. Draw Mode Content
    switch (mode) {
        case IslandMode::Idle:
            draw_idle(cr, width, height);
            break;
        case IslandMode::MusicCompact:
            draw_music_compact(cr, width, height, media, anim_time);
            break;
        case IslandMode::MusicExpanded:
            draw_music_expanded(cr, width, height, media, anim_time);
            break;
        case IslandMode::ControlCenter:
            draw_control_center(cr, width, height, cc_state, anim_time);
            break;
        case IslandMode::HudOSD:
            draw_hud_osd(cr, width, height, osd);
            break;
        case IslandMode::Notification:
            draw_notification(cr, width, height, notif);
            break;
    }

    cairo_restore(cr);

    // 3. Privacy Dot (Microphone Indicator)
    if (mic_active) {
        draw_privacy_dot(cr, width, height);
    }
}

void Renderer::draw_privacy_dot(cairo_t* cr, float w, float h) {
    (void)h;
    double dot_x = w - 12.0;
    double dot_y = 10.0;
    double dot_r = 2.8;

    // Subtle Coral Glow Ring
    cairo_arc(cr, dot_x, dot_y, dot_r + 2.0, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.95, 0.55, 0.51, 0.30);
    cairo_fill(cr);

    // Center Dot
    cairo_arc(cr, dot_x, dot_y, dot_r, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.98, 0.58, 0.52, 0.95);
    cairo_fill(cr);
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

    double dot_radius = 3.2;
    double spacing = 7.0;
    double total_content_w = (dot_radius * 2.0) + spacing + ink_rect.width;
    double start_x = (w - total_content_w) / 2.0;

    // 1. M3 Lavender Pulse Dot (#d0bcff)
    cairo_arc(cr, start_x + dot_radius, h / 2.0, dot_radius, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.92);
    cairo_fill(cr);

    // 2. Clock Text (M3 On-Surface #e6e0e9)
    double text_x = start_x + (dot_radius * 2.0) + spacing - ink_rect.x;
    double text_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.98);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time) {
    float left_margin = 8.0f;
    float right_margin = w - 8.0f;

    // 1. Left side: Mini Album Art if available
    if (mini_art_surface_) {
        float art_size = 22.0f;
        float art_x = left_margin;
        float art_y = (h - art_size) / 2.0f;

        cairo_save(cr);
        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 5.0);
        cairo_clip(cr);
        cairo_set_source_surface(cr, mini_art_surface_, art_x, art_y);
        cairo_paint(cr);
        cairo_restore(cr);

        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 5.0);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.35);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        left_margin += art_size + 8.0f;
    } else {
        float wave_x = left_margin + 2.0f;
        float wave_y = h / 2.0f;
        for (int i = 0; i < 3; ++i) {
            float bar_h = 4.0f + 6.0f * std::abs(std::sin(anim_time * 5.0f + i * 1.3f));
            if (!media.playing) bar_h = 3.0f;

            cairo_rectangle(cr, wave_x + i * 4.5f, wave_y - bar_h / 2.0f, 2.2f, bar_h);
            cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.92); // M3 Lavender
            cairo_fill(cr);
        }
        left_margin += 20.0f;
    }

    // 2. Right side: Equalizer or Music Icon
    float right_content_w = 0.0f;
    if (mini_art_surface_) {
        float wave_x = right_margin - 16.0f;
        float wave_y = h / 2.0f;
        for (int i = 0; i < 3; ++i) {
            float bar_h = 4.0f + 6.0f * std::abs(std::sin(anim_time * 6.0f + i * 1.5f));
            if (!media.playing) bar_h = 2.5f;

            cairo_rectangle(cr, wave_x + i * 4.5f, wave_y - bar_h / 2.0f, 2.2f, bar_h);
            cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.90);
            cairo_fill(cr);
        }
        right_content_w = 20.0f;
    } else {
        PangoLayout* icon_layout = pango_cairo_create_layout(cr);
        PangoFontDescription* icon_font = pango_font_description_from_string("JetBrainsMono Nerd Font 10");
        pango_layout_set_font_description(icon_layout, icon_font);
        pango_layout_set_text(icon_layout, "󰎆", -1);

        PangoRectangle ink_r, log_r;
        pango_layout_get_pixel_extents(icon_layout, &ink_r, &log_r);
        double icon_y = (h - ink_r.height) / 2.0 - ink_r.y;

        cairo_move_to(cr, right_margin - 14.0, icon_y);
        cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 0.85);
        pango_cairo_show_layout(cr, icon_layout);
        pango_font_description_free(icon_font);
        g_object_unref(icon_layout);
        right_content_w = 18.0f;
    }

    // 3. Center: Track title & Artist in M3 typography
    std::string display_text = media.title.empty() ? "Música" : media.title;
    if (!media.artist.empty()) {
        display_text += " • " + media.artist;
    }

    float available_w = (w - left_margin - right_content_w - 6.0f);
    if (available_w < 20.0f) available_w = 20.0f;

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("Sans SemiBold 9.5");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, display_text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, available_w * PANGO_SCALE);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);

    double text_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, left_margin, text_y);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.98); // M3 On-Surface
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
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
    char date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);
    strftime(date_str, sizeof(date_str), "%a, %d %b", tm_info);

    // 1. Header (Time, Date, Quick Actions on top right)
    PangoLayout* clock_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* clock_font = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 12.5");
    pango_layout_set_font_description(clock_layout, clock_font);
    pango_layout_set_text(clock_layout, time_str, -1);
    cairo_move_to(cr, 16.0, 13.0);
    cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 1.0); // M3 On-Surface
    pango_cairo_show_layout(cr, clock_layout);
    pango_font_description_free(clock_font);
    g_object_unref(clock_layout);

    PangoLayout* date_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* date_font = pango_font_description_from_string("Sans 9.0");
    pango_layout_set_font_description(date_layout, date_font);
    pango_layout_set_text(date_layout, date_str, -1);
    cairo_move_to(cr, 68.0, 16.0);
    cairo_set_source_rgba(cr, 0.792, 0.769, 0.816, 0.85); // M3 Muted
    pango_cairo_show_layout(cr, date_layout);
    pango_font_description_free(date_font);
    g_object_unref(date_layout);

    // Header Quick Action Buttons
    auto draw_header_btn = [&](float x, const char* icon, bool highlight) {
        float btn_sz = 22.0f;
        draw_rounded_rect(cr, x, 12.0f, btn_sz, btn_sz, 11.0);
        if (highlight) {
            cairo_set_source_rgba(cr, 0.310, 0.216, 0.545, 0.85); // Primary Container
        } else {
            cairo_set_source_rgba(cr, 0.169, 0.145, 0.212, 0.85);
        }
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.35);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        PangoLayout* ico = pango_cairo_create_layout(cr);
        PangoFontDescription* f = pango_font_description_from_string("JetBrainsMono Nerd Font 10");
        pango_layout_set_font_description(ico, f);
        pango_layout_set_text(ico, icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(ico, &ink, &log);
        cairo_move_to(cr, x + (btn_sz - ink.width) / 2.0 - ink.x,
                          12.0 + (btn_sz - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, highlight ? 0.918 : 0.80, highlight ? 0.867 : 0.78, 1.0, 0.95);
        pango_cairo_show_layout(cr, ico);
        pango_font_description_free(f);
        g_object_unref(ico);
    };

    // Close button (far right)
    draw_header_btn(w - 34.0f, "󰅖", false);
    // Lock button
    draw_header_btn(w - 60.0f, "󰌾", false);
    // Screenshot button
    draw_header_btn(w - 86.0f, "󰄀", false);
    // Media toggle button (if media active)
    if (state.media_active) {
        draw_header_btn(w - 112.0f, "󰎆", true);
    }

    // 2. Impasto System Stats Bar (CPU, RAM, Battery chips)
    float stats_y = 40.0f;
    float stat_chip_gap = 6.0f;
    float stat_chip_w = (w - 32.0f - stat_chip_gap * 2.0f) / 3.0f;
    float stat_chip_h = 24.0f;

    auto draw_stat_chip = [&](float x, const char* icon, const std::string& val_str, double r, double g, double b) {
        draw_rounded_rect(cr, x, stats_y, stat_chip_w, stat_chip_h, 7.0);
        cairo_set_source_rgba(cr, 0.125, 0.106, 0.165, 0.85); // M3 Surface Container
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.22);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Icon
        PangoLayout* il = pango_cairo_create_layout(cr);
        PangoFontDescription* ifont = pango_font_description_from_string("JetBrainsMono Nerd Font 9.5");
        pango_layout_set_font_description(il, ifont);
        pango_layout_set_text(il, icon, -1);
        PangoRectangle ink, log;
        pango_layout_get_pixel_extents(il, &ink, &log);
        cairo_move_to(cr, x + 7.0 - ink.x, stats_y + (stat_chip_h - ink.height) / 2.0 - ink.y);
        cairo_set_source_rgba(cr, r, g, b, 0.95);
        pango_cairo_show_layout(cr, il);
        pango_font_description_free(ifont);
        g_object_unref(il);

        // Value text
        PangoLayout* vl = pango_cairo_create_layout(cr);
        PangoFontDescription* vfont = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 8.5");
        pango_layout_set_font_description(vl, vfont);
        pango_layout_set_text(vl, val_str.c_str(), -1);
        PangoRectangle vink, vlog;
        pango_layout_get_pixel_extents(vl, &vink, &vlog);
        cairo_move_to(cr, x + 21.0, stats_y + (stat_chip_h - vink.height) / 2.0 - vink.y);
        cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.92);
        pango_cairo_show_layout(cr, vl);
        pango_font_description_free(vfont);
        g_object_unref(vl);
    };

    // CPU Stat
    std::string cpu_str = "CPU " + std::to_string(static_cast<int>(std::round(state.stats.cpu_percent))) + "%";
    draw_stat_chip(16.0f, "󰍛", cpu_str, 0.502, 0.871, 0.773); // Mint green

    // RAM Stat
    std::string ram_str = "RAM " + std::to_string(static_cast<int>(std::round(state.stats.ram_percent))) + "%";
    draw_stat_chip(16.0f + stat_chip_w + stat_chip_gap, "󰘚", ram_str, 0.816, 0.737, 1.0); // Lavender

    // Battery Stat
    std::string bat_str = std::to_string(state.battery_percent) + "%";
    draw_stat_chip(16.0f + (stat_chip_w + stat_chip_gap) * 2.0f,
                   state.battery_charging ? "󰂄" : "󰁹", bat_str, 1.0, 0.824, 0.490); // Amber

    // 3. Tonal Toggle Chips (2x2 Grid)
    float chip_w = (w - 32.0f - 10.0f) / 2.0f;
    float chip_h = 30.0f;
    float col1_x = 16.0f;
    float col2_x = 16.0f + chip_w + 10.0f;
    float row1_y = 70.0f;
    float row2_y = 106.0f;

    auto draw_chip = [&](float x, float y, const char* icon, const std::string& label, bool active, bool alert = false) {
        draw_rounded_rect(cr, x, y, chip_w, chip_h, 8.0);
        if (alert && active) {
            cairo_set_source_rgba(cr, 0.35, 0.12, 0.14, 0.90);
        } else if (active) {
            cairo_set_source_rgba(cr, 0.310, 0.216, 0.545, 0.90); // M3 Primary Container
        } else {
            cairo_set_source_rgba(cr, 0.125, 0.106, 0.165, 0.88); // M3 Surface Container
        }
        cairo_fill_preserve(cr);

        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, active ? 0.45 : 0.25);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Icon
        PangoLayout* i_layout = pango_cairo_create_layout(cr);
        PangoFontDescription* i_font = pango_font_description_from_string("JetBrainsMono Nerd Font 11");
        pango_layout_set_font_description(i_layout, i_font);
        pango_layout_set_text(i_layout, icon, -1);
        PangoRectangle i_ink, i_log;
        pango_layout_get_pixel_extents(i_layout, &i_ink, &i_log);
        cairo_move_to(cr, x + 9.0 - i_ink.x, y + (chip_h - i_ink.height) / 2.0 - i_ink.y);
        if (alert && active) {
            cairo_set_source_rgba(cr, 0.98, 0.58, 0.52, 1.0);
        } else if (active) {
            cairo_set_source_rgba(cr, 0.816, 0.737, 1.0, 1.0);
        } else {
            cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.85);
        }
        pango_cairo_show_layout(cr, i_layout);
        pango_font_description_free(i_font);
        g_object_unref(i_layout);

        // Label
        PangoLayout* l_layout = pango_cairo_create_layout(cr);
        PangoFontDescription* l_font = pango_font_description_from_string("Sans SemiBold 8.5");
        pango_layout_set_font_description(l_layout, l_font);
        pango_layout_set_text(l_layout, label.c_str(), -1);
        pango_layout_set_ellipsize(l_layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_width(l_layout, (chip_w - 36.0f) * PANGO_SCALE);

        PangoRectangle l_ink, l_log;
        pango_layout_get_pixel_extents(l_layout, &l_ink, &l_log);
        cairo_move_to(cr, x + 28.0, y + (chip_h - l_ink.height) / 2.0 - l_ink.y);
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
    draw_chip(col2_x, row2_y, state.dnd_enabled ? "󰍷" : "󰂚",
              state.dnd_enabled ? "No Molestar" : "Notificaciones",
              state.dnd_enabled);

    // 4. Impasto LevelRows (Volume & Brightness Sliders)
    float s_x = 16.0f;
    float s_w = w - 32.0f;
    float s_h = 26.0f;

    auto draw_slider = [&](float y, const char* icon, float val, double r_fill, double g_fill, double b_fill) {
        val = std::clamp(val, 0.0f, 1.0f);
        // Track
        draw_rounded_rect(cr, s_x, y, s_w, s_h, 13.0);
        cairo_set_source_rgba(cr, 0.125, 0.106, 0.165, 0.90);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, 0.40, 0.35, 0.48, 0.20);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        // Active fill
        if (val > 0.01f) {
            float fill_w = std::max(26.0f, s_w * val);
            draw_rounded_rect(cr, s_x, y, fill_w, s_h, 13.0);
            cairo_set_source_rgba(cr, r_fill, g_fill, b_fill, 0.95);
            cairo_fill(cr);
        }

        // Icon inside slider on the left
        PangoLayout* s_icon = pango_cairo_create_layout(cr);
        PangoFontDescription* si_font = pango_font_description_from_string("JetBrainsMono Nerd Font 11");
        pango_layout_set_font_description(s_icon, si_font);
        pango_layout_set_text(s_icon, icon, -1);
        PangoRectangle si_ink, si_log;
        pango_layout_get_pixel_extents(s_icon, &si_ink, &si_log);
        cairo_move_to(cr, s_x + 10.0 - si_ink.x, y + (s_h - si_ink.height) / 2.0 - si_ink.y);
        if (val > 0.15f) {
            cairo_set_source_rgba(cr, 0.15, 0.10, 0.22, 1.0); // High contrast on active bar
        } else {
            cairo_set_source_rgba(cr, r_fill, g_fill, b_fill, 0.95);
        }
        pango_cairo_show_layout(cr, s_icon);
        pango_font_description_free(si_font);
        g_object_unref(s_icon);

        // Percentage text inside slider on the right
        std::string pct_str = std::to_string(static_cast<int>(std::round(val * 100.0f))) + "%";
        PangoLayout* s_pct = pango_cairo_create_layout(cr);
        PangoFontDescription* sp_font = pango_font_description_from_string("JetBrainsMono Nerd Font Bold 9.0");
        pango_layout_set_font_description(s_pct, sp_font);
        pango_layout_set_text(s_pct, pct_str.c_str(), -1);
        PangoRectangle sp_ink, sp_log;
        pango_layout_get_pixel_extents(s_pct, &sp_ink, &sp_log);
        cairo_move_to(cr, s_x + s_w - 12.0 - sp_ink.width - sp_ink.x, y + (s_h - sp_ink.height) / 2.0 - sp_ink.y);
        cairo_set_source_rgba(cr, 0.902, 0.878, 0.914, 0.95);
        pango_cairo_show_layout(cr, s_pct);
        pango_font_description_free(sp_font);
        g_object_unref(s_pct);
    };

    // Volume Slider
    draw_slider(144.0f, state.audio_muted ? "󰝟" : "󰕾", state.volume, 0.816, 0.737, 1.0);

    // Brightness Slider
    draw_slider(176.0f, "󰃠", state.brightness, 1.0, 0.824, 0.490);

    // 5. Impasto Subtle System Identifier Footer
    float foot_y = 214.0f;
    PangoLayout* foot_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* foot_font = pango_font_description_from_string("JetBrainsMono Nerd Font 8.5");
    pango_layout_set_font_description(foot_layout, foot_font);
    pango_layout_set_text(foot_layout, "leimsoto@debian  •  niri", -1);
    PangoRectangle fi_ink, fi_log;
    pango_layout_get_pixel_extents(foot_layout, &fi_ink, &fi_log);
    cairo_move_to(cr, (w - fi_ink.width) / 2.0 - fi_ink.x, foot_y + 4.0);
    cairo_set_source_rgba(cr, 0.584, 0.557, 0.612, 0.70); // Subtle M3 Outline / Muted
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
