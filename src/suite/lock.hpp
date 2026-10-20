#pragma once

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/wayland.hpp"

namespace island {

// The lock screen, on ext-session-lock: the compositor keeps the session
// locked until we say so (even if the island were to die). Blurred
// wallpaper, the date and a big two-line clock, a small docked island with
// the padlock, and the user's capsule with the password. PAM checks it.
class Lock {
public:
    explicit Lock(Wayland& wl) : wl_(wl) {}
    ~Lock();

    // preview > 0: unlocks by itself after that many seconds (for trying it out).
    bool lock(double preview = 0);
    bool locked() const { return state_ == State::Locked; }
    bool active() const { return state_ != State::Idle; }
    void pump();
    bool wants_frame() const;

    std::string wallpaper;  // set by the app (the backdrop's image)
    static void on_locked_cb(void* data, ext_session_lock_v1*);
    static void on_finished_cb(void* data, ext_session_lock_v1*);
    std::function<void()> on_unlocked;
    std::function<void()> on_failed;  // the compositor refused (another locker running)

private:
    enum class State { Idle, Locking, Locked };
    struct Out {
        std::unique_ptr<LayerSurface> surf;
        bool dirty = true;
    };
    Wayland& wl_;
    State state_ = State::Idle;
    ext_session_lock_v1* lock_ = nullptr;
    std::vector<Out> outs_;
    std::string pass_;
    bool checking_ = false;
    double fail_at_ = -10, opened_at_ = 0, last_key_ = 0;
    uint64_t tick_ = 0, preview_timer_ = 0;
    cairo_surface_t* bg_ = nullptr;     // the wallpaper, small and blurred
    cairo_surface_t* avatar_ = nullptr;
    std::string user_, name_;

    void unlock();
    void request();
    void render(Out& o);
    void key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods);
    void submit();
};

}  // namespace island
