#include "renderer.hpp"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <cmath>
#include <ctime>
#include <iostream>

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
        std::cerr << "[Island] Failed to load art (" << path << "): " << (err ? err->message : "unknown") << "\n";
        if (err) g_error_free(err);
        return nullptr;
    }
    std::cout << "[Island] Loaded album art (" << target_w << "x" << target_h << ") for: " << path << "\n";

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

void Renderer::draw(cairo_t* cr, float width, float height, IslandMode mode, const MediaInfo& media, float anim_time) {
    update_art(media.art_url);

    double radius = std::min((double)height / 2.0, 22.0);

    // 1. Draw Island Body (OLED Deep Black with subtle outline)
    draw_rounded_rect(cr, 1.0, 1.0, width - 2.0, height - 2.0, radius);
    cairo_set_source_rgba(cr, 0.05, 0.05, 0.07, 0.96); // #0d0d12
    cairo_fill_preserve(cr);

    // Subtle Glass Edge
    cairo_set_source_rgba(cr, 0.35, 0.38, 0.48, 0.25);
    cairo_set_line_width(cr, 1.2);
    cairo_stroke(cr);

    // Clip content to rounded pill
    cairo_save(cr);
    draw_rounded_rect(cr, 2.0, 2.0, width - 4.0, height - 4.0, radius - 1.0);
    cairo_clip(cr);

    // 2. Draw Mode Content
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
    }

    cairo_restore(cr);
}

void Renderer::draw_idle(cairo_t* cr, float w, float h) {
    // Current time
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", tm_info);

    // Layout Clock Text
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("JetBrainsMono Nerd Font SemiBold 11");
    if (!font_desc) font_desc = pango_font_description_from_string("Sans SemiBold 11");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, time_str, -1);

    // Optical extents to align pixel-perfect vertical center
    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_pixel_extents(layout, &ink_rect, &logical_rect);

    // Perfectly balanced horizontal layout: [dot] (spacing) [clock text]
    double dot_radius = 3.5;
    double spacing = 8.0;
    double total_content_w = (dot_radius * 2.0) + spacing + ink_rect.width;
    double start_x = (w - total_content_w) / 2.0;

    // 1. Pulse dot
    cairo_arc(cr, start_x + dot_radius, h / 2.0, dot_radius, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.2, 0.85, 0.5, 0.92); // Emerald green
    cairo_fill(cr);

    // 2. Clock Text (Center optical ink height precisely with island Y center)
    double text_x = start_x + (dot_radius * 2.0) + spacing - ink_rect.x;
    double text_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, text_x, text_y);
    cairo_set_source_rgba(cr, 0.94, 0.95, 0.98, 0.96);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time) {
    float left_margin = 8.0f;
    float right_margin = w - 8.0f;

    // 1. Left side: Mini Album Art if available, otherwise audio bars
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

        // Subtle ring around mini art
        draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 5.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
        cairo_set_line_width(cr, 0.8);
        cairo_stroke(cr);

        left_margin += art_size + 8.0f;
    } else {
        // Fallback: Dancing audio waveform bars at left
        float wave_x = left_margin + 2.0f;
        float wave_y = h / 2.0f;
        for (int i = 0; i < 3; ++i) {
            float bar_h = 4.0f + 6.0f * std::abs(std::sin(anim_time * 5.0f + i * 1.3f));
            if (!media.playing) bar_h = 3.0f;

            cairo_rectangle(cr, wave_x + i * 4.5f, wave_y - bar_h / 2.0f, 2.2f, bar_h);
            cairo_set_source_rgba(cr, 0.25, 0.8, 0.95, 0.9);
            cairo_fill(cr);
        }
        left_margin += 22.0f;
    }

    // 2. Right side: Dancing audio wave (if art is present) or musical note icon
    float right_content_w = 0.0f;
    if (mini_art_surface_) {
        // Equalizer waveform on right like iOS
        float wave_x = right_margin - 16.0f;
        float wave_y = h / 2.0f;
        for (int i = 0; i < 3; ++i) {
            float bar_h = 4.0f + 6.0f * std::abs(std::sin(anim_time * 6.0f + i * 1.5f));
            if (!media.playing) bar_h = 2.5f;

            cairo_rectangle(cr, wave_x + i * 4.5f, wave_y - bar_h / 2.0f, 2.2f, bar_h);
            cairo_set_source_rgba(cr, 0.35, 0.85, 0.55, 0.95); // Neon green wave
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
        cairo_set_source_rgba(cr, 0.7, 0.75, 0.85, 0.8);
        pango_cairo_show_layout(cr, icon_layout);
        pango_font_description_free(icon_font);
        g_object_unref(icon_layout);
        right_content_w = 18.0f;
    }

    // 3. Center: Track title & Artist
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

    // Exact optical vertical centering
    double text_y = (h - ink_rect.height) / 2.0 - ink_rect.y;

    cairo_move_to(cr, left_margin, text_y);
    cairo_set_source_rgba(cr, 0.94, 0.95, 0.98, 0.96);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float) {
    (void)h;
    float art_x = 16.0f;
    float art_y = 16.0f;
    float art_size = 48.0f;

    // 1. Album Art box (rounded square)
    cairo_save(cr);
    draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 10.0);
    cairo_clip(cr);

    if (art_surface_) {
        cairo_set_source_surface(cr, art_surface_, art_x, art_y);
        cairo_paint(cr);
    } else {
        // Fallback placeholder with subtle gradient and note icon
        cairo_pattern_t* pat = cairo_pattern_create_linear(art_x, art_y, art_x + art_size, art_y + art_size);
        cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.18, 0.22, 0.32, 0.95);
        cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.08, 0.10, 0.16, 0.95);
        cairo_set_source(cr, pat);
        cairo_paint(cr);
        cairo_pattern_destroy(pat);

        // Art icon in center
        PangoLayout* art_icon = pango_cairo_create_layout(cr);
        PangoFontDescription* art_font = pango_font_description_from_string("JetBrainsMono Nerd Font 16");
        pango_layout_set_font_description(art_icon, art_font);
        pango_layout_set_text(art_icon, "󰎆", -1);

        PangoRectangle ink_r, log_r;
        pango_layout_get_pixel_extents(art_icon, &ink_r, &log_r);
        cairo_move_to(cr, art_x + (art_size - ink_r.width) / 2.0 - ink_r.x,
                          art_y + (art_size - ink_r.height) / 2.0 - ink_r.y);
        cairo_set_source_rgba(cr, 0.3, 0.8, 0.95, 0.85);
        pango_cairo_show_layout(cr, art_icon);
        pango_font_description_free(art_font);
        g_object_unref(art_icon);
    }
    cairo_restore(cr);

    // Subtle border over art
    draw_rounded_rect(cr, art_x, art_y, art_size, art_size, 10.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.15);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // 2. Song Title (Bold)
    float info_x = art_x + art_size + 14.0f;
    float info_w = w - info_x - 16.0f;

    PangoLayout* title_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* title_font = pango_font_description_from_string("Sans Bold 11");
    pango_layout_set_font_description(title_layout, title_font);
    pango_layout_set_text(title_layout, media.title.empty() ? "Sin reproducción" : media.title.c_str(), -1);
    pango_layout_set_ellipsize(title_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(title_layout, info_w * PANGO_SCALE);

    PangoRectangle title_ink, title_log;
    pango_layout_get_pixel_extents(title_layout, &title_ink, &title_log);

    // Vertically positioned in the upper half of the cover art
    double title_y = art_y + 8.0 - title_ink.y;
    cairo_move_to(cr, info_x, title_y);
    cairo_set_source_rgba(cr, 0.98, 0.98, 1.0, 1.0);
    pango_cairo_show_layout(cr, title_layout);
    pango_font_description_free(title_font);
    g_object_unref(title_layout);

    // 3. Artist (Muted)
    PangoLayout* artist_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* artist_font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(artist_layout, artist_font);
    pango_layout_set_text(artist_layout, media.artist.empty() ? "Desconocido" : media.artist.c_str(), -1);
    pango_layout_set_ellipsize(artist_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(artist_layout, info_w * PANGO_SCALE);

    PangoRectangle artist_ink, artist_log;
    pango_layout_get_pixel_extents(artist_layout, &artist_ink, &artist_log);

    // Vertically positioned in the lower half of the cover art
    double artist_y = art_y + 28.0 - artist_ink.y;
    cairo_move_to(cr, info_x, artist_y);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.78, 0.85);
    pango_cairo_show_layout(cr, artist_layout);
    pango_font_description_free(artist_font);
    g_object_unref(artist_layout);

    // 4. Progress Bar
    float bar_x = 16.0f;
    float bar_y = 74.0f;
    float bar_w = w - 32.0f;
    float bar_h = 3.5f;

    // Track background
    draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 2.0);
    cairo_set_source_rgba(cr, 0.22, 0.24, 0.32, 0.7);
    cairo_fill(cr);

    // Active progress
    draw_rounded_rect(cr, bar_x, bar_y, bar_w * 0.45f, bar_h, 2.0);
    cairo_set_source_rgba(cr, 0.3, 0.8, 0.95, 0.95);
    cairo_fill(cr);

    // 5. Media Control Buttons (Previous, Play/Pause, Next)
    PangoLayout* btn_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* btn_font = pango_font_description_from_string("JetBrainsMono Nerd Font 13");
    pango_layout_set_font_description(btn_layout, btn_font);

    // Centered button row area (between y=80 and y=110)
    double btn_center_y = 95.0;

    // Previous 󰒮
    pango_layout_set_text(btn_layout, "󰒮", -1);
    PangoRectangle prev_ink, prev_log;
    pango_layout_get_pixel_extents(btn_layout, &prev_ink, &prev_log);
    cairo_move_to(cr, w / 2.0 - 55.0 - prev_ink.width / 2.0 - prev_ink.x,
                      btn_center_y - prev_ink.height / 2.0 - prev_ink.y);
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.9, 0.9);
    pango_cairo_show_layout(cr, btn_layout);

    // Play/Pause 󰏤 / 󰐊
    pango_layout_set_text(btn_layout, media.playing ? "󰏤" : "󰐊", -1);
    PangoRectangle play_ink, play_log;
    pango_layout_get_pixel_extents(btn_layout, &play_ink, &play_log);
    cairo_move_to(cr, w / 2.0 - play_ink.width / 2.0 - play_ink.x,
                      btn_center_y - play_ink.height / 2.0 - play_ink.y);
    cairo_set_source_rgba(cr, 0.3, 0.85, 0.95, 1.0);
    pango_cairo_show_layout(cr, btn_layout);

    // Next 󰒭
    pango_layout_set_text(btn_layout, "󰒭", -1);
    PangoRectangle next_ink, next_log;
    pango_layout_get_pixel_extents(btn_layout, &next_ink, &next_log);
    cairo_move_to(cr, w / 2.0 + 55.0 - next_ink.width / 2.0 - next_ink.x,
                      btn_center_y - next_ink.height / 2.0 - next_ink.y);
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.9, 0.9);
    pango_cairo_show_layout(cr, btn_layout);

    pango_font_description_free(btn_font);
    g_object_unref(btn_layout);
}

} // namespace island
