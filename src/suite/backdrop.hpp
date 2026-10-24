#pragma once

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>

#include "core/proc.hpp"
#include "core/wayland.hpp"
#include "suite/webwall.hpp"

namespace island {

// The wallpaper itself: a background-layer surface showing the image (cropped
// to fill); niri-island-engine for videos and Wallpaper Engine scenes and
// videos (a Workshop item's folder); WebWall for web ones; mpvpaper for
// videos when the engine is not built. It remembers the last one, and makes the
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
    void outputs_changed();               // a screen plugged in or out
    void set_paused(bool paused);         // a video wallpaper stops while hidden
    void set_audio(bool on);              // a Wallpaper Engine one's sound
    // A picture of the current wallpaper (a video's frame, an item's preview).
    std::string still() const;

    static std::string state_path();      // {"path": …}
    static std::string palette_path();    // colors.json for the theme

    std::function<void()> on_palette;     // the palette file was rewritten
    std::function<void(const std::string&)> on_error;
    std::function<void(const std::string& path)> on_shown;  // the new wallpaper is on screen

private:
    Wayland& wl_;
    // One surface per screen (each gets the picture, covering it).
    struct Out {
        wl_output* output = nullptr;
        std::unique_ptr<LayerSurface> surf;
        bool dirty = true;
    };
    std::vector<Out> outs_;
    cairo_surface_t* img_ = nullptr;
    std::string path_;
    void mark_dirty() {
        for (auto& o : outs_) o.dirty = true;
    }
    uint64_t gen_ = 0;  // newest request wins when loads overlap
    proc::Child video_;

    void ensure_surface();
    void drop_surface();
    void stop_video();
    bool paused_ = false;
    // who draws what is on screen; who else may still be running
    enum class Drawer { Own, Mpv, Engine, Web };
    Drawer drawer_ = Drawer::Own;
    bool engine_on_ = false;
    std::unique_ptr<WebWall> web_;
    void retire(Drawer keep);
    void handed_over(Drawer to, uint64_t gen, const std::string& path, double settle);
    void send_mpv(const std::string& cmd);
    void render(Out& o);
    // Once on every screen, the full picture (a 4K PNG is 33 MB decoded) is
    // swapped for a copy just big enough for the screens: the same pixels
    // end up on screen. A bigger screen plugged in reloads the file.
    bool fitted_ = false;
    void fit_to_screens();
    void save_state() const;
    void make_palette(const std::string& path, bool video);
};

}  // namespace island
