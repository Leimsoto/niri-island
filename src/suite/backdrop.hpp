#pragma once

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>

#include "core/proc.hpp"
#include "core/wayland.hpp"

namespace island {

// The wallpaper itself: a background-layer surface showing the image (cropped
// to fill), or mpvpaper for videos. It remembers the last one, and makes the
// colour palette the island themes itself with (matugen, or the image's
// dominant colour when matugen is missing).
class Backdrop {
public:
    explicit Backdrop(Wayland& wl) : wl_(wl) {}
    ~Backdrop();

    void init();                          // restore the last wallpaper
    void set(const std::string& path);    // apply now, no transition
    void step(int dir);                   // next / previous in the wallpaper folders
    const std::string& current() const { return path_; }
    void pump();                          // draws when needed (from the loop)
    void set_paused(bool paused);         // a video wallpaper stops while hidden

    static std::string state_path();      // {"path": …}
    static std::string palette_path();    // colors.json for the theme

    std::function<void()> on_palette;     // the palette file was rewritten
    std::function<void(const std::string&)> on_error;
    std::function<void(const std::string& path)> on_shown;  // the new wallpaper is on screen

private:
    Wayland& wl_;
    std::unique_ptr<LayerSurface> surf_;
    cairo_surface_t* img_ = nullptr;
    std::string path_;
    bool dirty_ = false;
    uint64_t gen_ = 0;  // newest request wins when loads overlap
    proc::Child video_;

    void ensure_surface();
    void drop_surface();
    void stop_video();
    bool paused_ = false;
    void send_mpv(const std::string& cmd);
    void render();
    void save_state() const;
    void make_palette(const std::string& path, bool video);
};

}  // namespace island
