#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/proc.hpp"
#include "core/wayland.hpp"

struct wp_viewport;
struct wl_buffer;

namespace island {

// Wallpaper Engine web wallpapers: waywallen's CEF renderer
// (waywallen-weweb-renderer, from the open-wallpaper-engine plugin) drives
// a page and hands its frames over as dma-bufs; we play its daemon (the
// waywallen_ipc_v3 wire, through the bridge's codec) and put the frames
// straight on our own background surfaces: no copy, no waywallen.
//
// Each frame comes with a fence (wait for it before showing) and a point on
// the renderer's timeline to signal once the compositor lets the buffer go.
class WebWall {
public:
    explicit WebWall(Wayland& wl) : wl_(wl) {}
    ~WebWall();

    // The renderer and the bridge library it loads; empty when missing.
    static std::string renderer();
    static std::string bridge_dir();
    static bool available() { return !renderer().empty() && !bridge_dir().empty(); }
    // Fetches the renderer from open-wallpaper-engine's latest release (its
    // lib/weweb, ~340 MB unpacked) into ~/.local/share/niri-island/weweb.
    // Blocking: from a worker.
    static bool install_renderer(std::string* error);

    bool start(const std::string& dir, bool muted);  // false: cannot run it
    void stop();
    bool running() const { return child_.pid > 0; }
    void set_paused(bool paused);
    void set_muted(bool muted);

    std::function<void()> on_shown;                    // its first frame is on screen
    std::function<void(const std::string&)> on_error;  // it failed or went

private:
    Wayland& wl_;
    proc::Child child_;
    std::string sock_path_;
    int listen_fd_ = -1, sock_ = -1;
    bool stopping_ = false, shown_ = false, paused_ = false, muted_ = true;
    uint64_t kill_timer_ = 0;

    // the renderer's release timeline, imported on our render node; a point
    // is signalled only once every frame up to it is let go (the timeline
    // only moves forward, and a later point frees the earlier ones too)
    int drm_fd_ = -1;
    uint32_t release_handle_ = 0;
    uint32_t drm_minor_ = 0;
    std::vector<std::pair<uint64_t, bool>> points_;  // outstanding, oldest first: (point, done)
    void done(uint64_t point);

    // its pool: one wl_buffer per slot per screen
    struct Slot {
        std::vector<wl_buffer*> wl;  // per screen
        int busy = 0;                // screens still holding it
        uint64_t release_point = 0;  // to signal once nobody does
        bool pending = false;
    };
    std::vector<Slot> slots_;
    int buf_w_ = 0, buf_h_ = 0;
    int pending_buffers_ = 0;  // still being created by the compositor
    struct BufRef {            // what a wl_buffer's release tells us
        WebWall* self;
        int slot;
    };
    std::vector<std::unique_ptr<BufRef>> refs_;
    friend void webwall_params_created(void*, struct zwp_linux_buffer_params_v1*, wl_buffer*);
    friend void webwall_params_failed(void*, struct zwp_linux_buffer_params_v1*);

    // one background surface per screen
    struct Out {
        wl_output* output = nullptr;
        std::unique_ptr<LayerSurface> surf;
        wp_viewport* viewport = nullptr;
        int shown = -1;  // the slot it shows
    };
    std::vector<Out> outs_;

    // a frame waiting on its fence (a newer one replaces it)
    int wait_fd_ = -1;
    int wait_index_ = -1;
    uint64_t wait_point_ = 0;

    void accept_renderer();
    void readable();
    void send_init();
    void send_control(uint16_t op);  // play, pause, mute, unmute, shutdown
    void negotiate(const void* caps);
    void bind(const void* pool, const int* fds, size_t nfds);
    void frame_ready(uint32_t index, uint64_t point, int fence);
    void present(int index, uint64_t point);
    void released(int index);
    void drop_wait();
    void free_pool();
    void ensure_surfaces();
    void fit(Out& o);
    void failed(const std::string& why);
    friend void webwall_buffer_release(void*, wl_buffer*);
};

}  // namespace island
