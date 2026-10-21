#pragma once

#include <cairo.h>
#include <functional>
#include <string>
#include <vector>

#include "suite/overlay.hpp"

namespace island {

// The login screen (a greetd greeter), in the same style as the lock screen:
// blurred wallpaper, the date and a big two-line clock, the docked island,
// and the user's capsule. Users and sessions come from the system; the last
// choice is remembered. In demo mode it runs over the current desktop and
// only checks a fixed word, so it can be tried without logging out.
class Greeter : public Overlay {
public:
    struct Options {
        bool demo = false;
        std::string socket;     // $GREETD_SOCK
        std::string wallpaper;  // the backdrop
    };
    Greeter(Wayland& wl, Options opt);
    ~Greeter() override;

    void start();
    // The other screens: the blurred wallpaper, so none stays bare.
    void pump_extras();
    bool extras_want_frame() const;
    std::function<void()> on_done;  // a session was started (or demo left): exit

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override;
    void key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) override;
    bool animating() const override;
    const char* name() const override { return "niri-island-greeter"; }
    // the login itself on the first screen
    void adjust_spec(LayerSpec& s) const override {
        if (!wl_.outputs().empty()) s.output = wl_.outputs().front();
    }

private:
    struct User {
        std::string login, name, home;
        cairo_surface_t* avatar = nullptr;
    };
    struct Session {
        std::string name, exec, desktop;  // desktop: its DesktopNames
    };
    Options opt_;
    std::vector<User> users_;
    std::vector<Session> sessions_;
    int user_ = 0, session_ = 0;
    std::string pass_;
    bool checking_ = false;
    std::string error_, info_;
    double fail_at_ = -10, last_key_ = 0, msg_at_ = -10;
    cairo_surface_t* bg_ = nullptr;
    uint64_t tick_ = 0;
    std::string host_;
    struct Extra {
        std::unique_ptr<LayerSurface> surf;
        bool dirty = true;
    };
    std::vector<Extra> extras_;
    void paint_backdrop(cairo_t* cr, float W, float H) const;

    void load_users();
    void load_sessions();
    void load_state();
    void save_state() const;
    void submit();
    void power(const char* what);
    void message(const std::string& m);
};

}  // namespace island
