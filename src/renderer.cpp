#include "renderer.hpp"
#include <cmath>
#include <ctime>
#include <iostream>

namespace island {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

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

    // Pulse dot
    cairo_arc(cr, 22.0, h / 2.0, 3.5, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.2, 0.85, 0.5, 0.9); // Emerald green
    cairo_fill(cr);

    // Clock Text
    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("JetBrainsMono Nerd Font SemiBold 11");
    if (!font_desc) font_desc = pango_font_description_from_string("Sans SemiBold 11");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, time_str, -1);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_move_to(cr, (w - text_w) / 2.0 + 8.0, (h - text_h) / 2.0);
    cairo_set_source_rgba(cr, 0.92, 0.93, 0.96, 0.95);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_music_compact(cairo_t* cr, float w, float h, const MediaInfo& media, float anim_time) {
    // Left: Dancing audio waveform bars
    float wave_x = 18.0f;
    float wave_y = h / 2.0f;
    for (int i = 0; i < 3; ++i) {
        float bar_h = 4.0f + 7.0f * std::abs(std::sin(anim_time * 5.0f + i * 1.3f));
        if (!media.playing) bar_h = 3.0f;

        cairo_rectangle(cr, wave_x + i * 5.0f, wave_y - bar_h / 2.0f, 2.5f, bar_h);
        cairo_set_source_rgba(cr, 0.25, 0.8, 0.95, 0.9); // Cyan
        cairo_fill(cr);
    }

    // Right: small disc/note icon
    PangoLayout* icon_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* icon_font = pango_font_description_from_string("JetBrainsMono Nerd Font 10");
    pango_layout_set_font_description(icon_layout, icon_font);
    pango_layout_set_text(icon_layout, "󰎆", -1);
    cairo_move_to(cr, w - 24.0, (h - 14.0) / 2.0);
    cairo_set_source_rgba(cr, 0.7, 0.75, 0.85, 0.8);
    pango_cairo_show_layout(cr, icon_layout);
    pango_font_description_free(icon_font);
    g_object_unref(icon_layout);

    // Center: Track title
    std::string display_text = media.title.empty() ? "Música" : media.title;
    if (!media.artist.empty()) {
        display_text += " • " + media.artist;
    }

    PangoLayout* layout = pango_cairo_create_layout(cr);
    PangoFontDescription* font_desc = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_text(layout, display_text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (w - 75.0) * PANGO_SCALE);

    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);

    cairo_move_to(cr, 42.0, (h - text_h) / 2.0);
    cairo_set_source_rgba(cr, 0.94, 0.95, 0.98, 0.95);
    pango_cairo_show_layout(cr, layout);

    pango_font_description_free(font_desc);
    g_object_unref(layout);
}

void Renderer::draw_music_expanded(cairo_t* cr, float w, float h, const MediaInfo& media, float) {
    (void)h;
    // 1. Album Art box placeholder (rounded square)
    draw_rounded_rect(cr, 16.0, 16.0, 48.0, 48.0, 10.0);
    cairo_set_source_rgba(cr, 0.16, 0.18, 0.24, 0.9);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.3, 0.35, 0.45, 0.4);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // Art icon in center
    PangoLayout* art_icon = pango_cairo_create_layout(cr);
    PangoFontDescription* art_font = pango_font_description_from_string("JetBrainsMono Nerd Font 18");
    pango_layout_set_font_description(art_icon, art_font);
    pango_layout_set_text(art_icon, "󰎆", -1);
    cairo_move_to(cr, 30.0, 28.0);
    cairo_set_source_rgba(cr, 0.3, 0.8, 0.95, 0.9);
    pango_cairo_show_layout(cr, art_icon);
    pango_font_description_free(art_font);
    g_object_unref(art_icon);

    // 2. Song Title (Bold)
    PangoLayout* title_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* title_font = pango_font_description_from_string("Sans Bold 11");
    pango_layout_set_font_description(title_layout, title_font);
    pango_layout_set_text(title_layout, media.title.empty() ? "Sin reproducción" : media.title.c_str(), -1);
    pango_layout_set_ellipsize(title_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(title_layout, (w - 85.0) * PANGO_SCALE);

    cairo_move_to(cr, 76.0, 20.0);
    cairo_set_source_rgba(cr, 0.98, 0.98, 1.0, 1.0);
    pango_cairo_show_layout(cr, title_layout);
    pango_font_description_free(title_font);
    g_object_unref(title_layout);

    // 3. Artist / Album (Muted)
    PangoLayout* artist_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* artist_font = pango_font_description_from_string("Sans 10");
    pango_layout_set_font_description(artist_layout, artist_font);
    pango_layout_set_text(artist_layout, media.artist.empty() ? "Desconocido" : media.artist.c_str(), -1);
    pango_layout_set_ellipsize(artist_layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(artist_layout, (w - 85.0) * PANGO_SCALE);

    cairo_move_to(cr, 76.0, 42.0);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.78, 0.85);
    pango_cairo_show_layout(cr, artist_layout);
    pango_font_description_free(artist_font);
    g_object_unref(artist_layout);

    // 4. Progress Bar
    float bar_x = 18.0f;
    float bar_y = 74.0f;
    float bar_w = w - 36.0f;
    float bar_h = 3.5f;

    // Track background
    draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 2.0);
    cairo_set_source_rgba(cr, 0.22, 0.24, 0.32, 0.7);
    cairo_fill(cr);

    // Active progress (simulated 45%)
    draw_rounded_rect(cr, bar_x, bar_y, bar_w * 0.45f, bar_h, 2.0);
    cairo_set_source_rgba(cr, 0.3, 0.8, 0.95, 0.95);
    cairo_fill(cr);

    // 5. Media Control Buttons (Previous, Play/Pause, Next)
    PangoLayout* btn_layout = pango_cairo_create_layout(cr);
    PangoFontDescription* btn_font = pango_font_description_from_string("JetBrainsMono Nerd Font 13");
    pango_layout_set_font_description(btn_layout, btn_font);

    // Previous 󰒮
    pango_layout_set_text(btn_layout, "󰒮", -1);
    cairo_move_to(cr, w / 2.0 - 45.0, 84.0);
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.9, 0.9);
    pango_cairo_show_layout(cr, btn_layout);

    // Play/Pause 󰐊 / 󰏤
    pango_layout_set_text(btn_layout, media.playing ? "󰏤" : "󰐊", -1);
    cairo_move_to(cr, w / 2.0 - 7.0, 84.0);
    cairo_set_source_rgba(cr, 0.3, 0.85, 0.95, 1.0);
    pango_cairo_show_layout(cr, btn_layout);

    // Next 󰒭
    pango_layout_set_text(btn_layout, "󰒭", -1);
    cairo_move_to(cr, w / 2.0 + 30.0, 84.0);
    cairo_set_source_rgba(cr, 0.8, 0.82, 0.9, 0.9);
    pango_cairo_show_layout(cr, btn_layout);

    pango_font_description_free(btn_font);
    g_object_unref(btn_layout);
}

} // namespace island
