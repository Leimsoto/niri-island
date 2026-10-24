#include "suite/webwall.hpp"

#include <climits>
#include <csignal>
#include <cstring>
#include <drm/drm.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

extern "C" {
#include <waywallen-bridge/bridge.h>
#include <waywallen-bridge/ipc_v3.h>
#include <waywallen-bridge/protocol_bits.h>
}

#include "core/loop.hpp"
#include "core/util.hpp"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

namespace island {

namespace {

constexpr uint64_t kLinear = 0;  // DRM_FORMAT_MOD_LINEAR
// opaque first: a wallpaper has nothing behind it
constexpr uint32_t kFormats[] = {0x34325258 /* XR24 */, 0x34324258 /* XB24 */, 0x34325241 /* AR24 */, 0x34324241 /* AB24 */};

std::string exe_dir() {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return "";
    std::string d(self, static_cast<size_t>(n));
    return d.substr(0, d.rfind('/'));
}

// One message through the bridge's codec.
template <class M>
bool send_msg(int sock, uint16_t op, const M& m, int (*encode)(const M*, ww_buf_t*), const int* fds = nullptr,
              size_t nfds = 0) {
    ww_buf_t b;
    ww_buf_init(&b);
    bool ok = encode(&m, &b) == 0 && ww_bridge_send_frame(sock, op, b.data, b.len, fds, nfds) == 0;
    ww_buf_free(&b);
    return ok;
}

struct Pending {  // a buffer the compositor is creating: which slot, which screen
    WebWall* self;
    int slot, out;
};

}  // namespace

// ── where things are ──────────────────────────────────────────────────────

std::string WebWall::renderer() {
    const char* name = "waywallen-weweb-renderer";
    std::string home = home_dir();
    for (std::string dir : {home + "/.local/share/niri-island/weweb",
                            home + "/.var/app/org.waywallen.waywallen/data/waywallen/plugins/org.waywallen.open-wallpaper-engine/lib/weweb",
                            home + "/.local/share/waywallen/plugins/org.waywallen.open-wallpaper-engine/lib/weweb"})
        if (access((dir + "/" + name).c_str(), X_OK) == 0) return dir + "/" + name;
    return "";
}

std::string WebWall::bridge_dir() {
    std::string exe = exe_dir();
    for (std::string dir : {exe, exe + "/../lib/niri-island", home_dir() + "/.local/lib/niri-island"})
        if (file_exists(dir + "/libwaywallen-bridge.so")) return dir;
    return "";
}

// ── lifetime ──────────────────────────────────────────────────────────────

WebWall::~WebWall() {
    stop();
    if (child_.pid > 0) {
        kill(-child_.pid, SIGKILL);
        if (child_.pidfd >= 0) Loop::get().unwatch(child_.pidfd);
        proc::reap(child_);
    }
}

bool WebWall::start(const std::string& dir, bool muted) {
    stop();
    std::string bin = renderer(), lib = bridge_dir();
    if (bin.empty() || lib.empty() || !wl_.dmabuf_ || !wl_.viewporter_) return false;
    stopping_ = shown_ = paused_ = false;
    muted_ = muted;
    const char* rt = getenv("XDG_RUNTIME_DIR");
    sock_path_ = std::string(rt ? rt : "/tmp") + "/niri-island-web-" + std::to_string(getpid()) + ".sock";
    unlink(sock_path_.c_str());
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path_.c_str());
    if (listen_fd_ < 0 || ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(listen_fd_, 1) < 0) {
        if (listen_fd_ >= 0) close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    Loop::get().watch(listen_fd_, POLLIN, [this](short) { accept_renderer(); });
    std::string id = dir.substr(dir.rfind('/') + 1);
    child_ = proc::start({"/usr/bin/env", "LD_LIBRARY_PATH=" + lib, "WW_LOG=warn", bin, "--ipc", sock_path_, "--path", dir,
                          "--workshop_id", id},
                         cache_dir() + "/web.log");
    if (child_.pid <= 0) {
        stop();
        return false;
    }
    if (child_.pidfd >= 0)
        Loop::get().watch(child_.pidfd, POLLIN, [this](short) {
            Loop::get().unwatch(child_.pidfd);
            proc::reap(child_);
            Loop::get().cancel(kill_timer_);
            if (!stopping_) failed("El fondo web se cerró");
        });
    ensure_surfaces();
    return true;
}

void WebWall::stop() {
    if (stopping_ && sock_ < 0 && listen_fd_ < 0) return;
    stopping_ = true;
    if (sock_ >= 0) {
        send_control(WW_EVT_IN_SHUTDOWN);
        Loop::get().unwatch(sock_);
        close(sock_);
        sock_ = -1;
    }
    if (listen_fd_ >= 0) {
        Loop::get().unwatch(listen_fd_);
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(sock_path_.c_str());
    }
    drop_wait();
    for (auto& o : outs_)
        if (o.viewport) wp_viewport_destroy(o.viewport);
    outs_.clear();  // the surfaces go first: then nothing holds the buffers
    free_pool();
    points_.clear();
    if (drm_fd_ >= 0) {
        if (release_handle_) {
            drm_syncobj_destroy d{release_handle_, 0};
            ioctl(drm_fd_, DRM_IOCTL_SYNCOBJ_DESTROY, &d);
        }
        close(drm_fd_);
    }
    drm_fd_ = -1;
    release_handle_ = 0;
    // a moment to leave on its own (it was told), then its whole group goes
    if (child_.pid > 0) {
        int pid = child_.pid;
        Loop::get().cancel(kill_timer_);
        kill_timer_ = Loop::get().after(1.5, [pid] { kill(-pid, SIGTERM); });
    }
}

void WebWall::failed(const std::string& why) {
    bool was = !stopping_;
    stop();
    if (was && on_error) on_error(why);
}

void WebWall::set_paused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    send_control(paused ? WW_EVT_IN_PAUSE : WW_EVT_IN_PLAY);
}

void WebWall::set_muted(bool muted) {
    if (muted == muted_) return;
    muted_ = muted;
    send_control(muted ? WW_EVT_IN_MUTE : WW_EVT_IN_UNMUTE);
}

// ── the wire ──────────────────────────────────────────────────────────────

void WebWall::accept_renderer() {
    int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) return;
    Loop::get().unwatch(listen_fd_);
    close(listen_fd_);
    listen_fd_ = -1;
    unlink(sock_path_.c_str());
    sock_ = fd;
    Loop::get().watch(sock_, POLLIN, [this](short ev) {
        if (ev & (POLLHUP | POLLERR)) failed("El fondo web se cerró");
        else readable();
    });
    send_init();
}

void WebWall::send_init() {
    // its height on the tallest screen (16:9 at that short edge)
    int tall = 1080;
    for (auto& o : outs_)
        if (o.surf && o.surf->configured()) tall = std::max(tall, o.surf->height() * o.surf->scale());
    std::vector<std::pair<std::string, std::string>> kv = {
        {"enable_audio", "true"}, {"volume", "100"}, {"fps", "30"}, {"resolution", "-1"},
        {"custom_extent", std::to_string(tall)}, {"shared_texture_enabled", "true"}};
    std::vector<ww_kv_t> items;
    for (auto& [k, v] : kv) items.push_back({const_cast<char*>(k.c_str()), const_cast<char*>(v.c_str())});
    ww_evt_in_init_t m{};
    m.config.protocol_version = WW_BRIDGE_SUPPORTED_PROTOCOL_VERSION;
    m.config.spawn_version = WW_BRIDGE_SUPPORTED_SPAWN_VERSION;
    m.config.settings.count = static_cast<uint32_t>(items.size());
    m.config.settings.data = items.data();
    m.config.user_properties = const_cast<char*>("");
    send_msg(sock_, WW_EVT_IN_INIT, m, ww_evt_in_init_encode);
}

void WebWall::send_control(uint16_t op) {
    if (sock_ < 0) return;
    switch (op) {
        case WW_EVT_IN_PLAY: send_msg(sock_, op, ww_evt_in_play_t{}, ww_evt_in_play_encode); break;
        case WW_EVT_IN_PAUSE: send_msg(sock_, op, ww_evt_in_pause_t{}, ww_evt_in_pause_encode); break;
        case WW_EVT_IN_MUTE: send_msg(sock_, op, ww_evt_in_mute_t{}, ww_evt_in_mute_encode); break;
        case WW_EVT_IN_UNMUTE: send_msg(sock_, op, ww_evt_in_unmute_t{}, ww_evt_in_unmute_encode); break;
        case WW_EVT_IN_SHUTDOWN: send_msg(sock_, op, ww_evt_in_shutdown_t{}, ww_evt_in_shutdown_encode); break;
        default: break;
    }
}

void WebWall::readable() {
    uint16_t op = 0;
    uint8_t* body = nullptr;
    size_t len = 0, nfds = 0;
    int fds[64];
    if (ww_bridge_recv_frame(sock_, &op, &body, &len, fds, 64, &nfds) != 0) {
        failed("El fondo web se cerró");
        return;
    }
    size_t used = 0;  // fds taken by the handler (the rest are closed)
    switch (op) {
        case WW_EVT_READY: {
            ww_evt_ready_t m{};
            if (ww_evt_ready_decode(body, len, &m) == 0) {
                drm_minor_ = m.drm_node.minor;
                ww_evt_ready_free(&m);
            }
            std::string node = "/dev/dri/renderD" + std::to_string(drm_minor_ >= 128 ? drm_minor_ : 128);
            drm_fd_ = open(node.c_str(), O_RDWR | O_CLOEXEC);
            break;
        }
        case WW_EVT_RELEASE_SYNCOBJ:
            if (nfds >= 1 && drm_fd_ >= 0) {
                drm_syncobj_handle h{};
                h.fd = fds[0];
                if (ioctl(drm_fd_, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &h) == 0) release_handle_ = h.handle;
            }
            break;
        case WW_EVT_FORMAT_CAPS: {
            ww_evt_format_caps_t m{};
            if (ww_evt_format_caps_decode(body, len, &m) == 0) {
                negotiate(&m.capabilities);
                ww_evt_format_caps_free(&m);
            }
            break;
        }
        case WW_EVT_BIND_BUFFERS: {
            ww_evt_bind_buffers_t m{};
            if (ww_evt_bind_buffers_decode(body, len, &m) == 0) {
                bind(&m.pool, fds, nfds);
                ww_evt_bind_buffers_free(&m);
            }
            break;
        }
        case WW_EVT_FRAME_READY: {
            ww_evt_frame_ready_t m{};
            if (ww_evt_frame_ready_decode(body, len, &m) == 0) {
                frame_ready(m.frame.image_index, m.frame.release_point, nfds >= 1 ? fds[0] : -1);
                if (nfds >= 1) used = 1;  // the fence is ours now
                ww_evt_frame_ready_free(&m);
            }
            break;
        }
        case WW_EVT_ERROR: {
            ww_evt_error_t m{};
            std::string msg = "El fondo web falló";
            if (ww_evt_error_decode(body, len, &m) == 0) {
                if (m.message && *m.message) msg = m.message;
                ww_evt_error_free(&m);
            }
            free(body);
            for (size_t i = 0; i < nfds; ++i) close(fds[i]);
            failed(msg);
            return;
        }
        case WW_EVT_BIND_FAILED:
        case WW_EVT_INIT_NACK:
            free(body);
            for (size_t i = 0; i < nfds; ++i) close(fds[i]);
            failed("El fondo web no pudo empezar");
            return;
        default: break;  // state reports, subscriptions: nothing we need
    }
    free(body);
    for (size_t i = used; i < nfds; ++i) close(fds[i]);
}

// Its formats against what the compositor takes: XRGB-ish, optimal tiling
// when both do it, else linear (which any GPU can show).
void WebWall::negotiate(const void* caps_v) {
    const auto& caps = *static_cast<const waywallen_producer_capabilities_t*>(caps_v);
    auto takes = [&](uint32_t f, uint64_t m) {
        if (wl_.dmabuf_formats_.empty()) return m == kLinear;
        for (auto& [ff, mm] : wl_.dmabuf_formats_)
            if (ff == f && mm == m) return true;
        return false;
    };
    int best = -1;
    uint32_t best_f = 0, planes = 1;
    uint64_t best_m = 0;
    size_t at = 0;
    for (uint32_t i = 0; i < caps.fourccs.count; ++i) {
        uint32_t f = caps.fourccs.data[i];
        uint32_t n = i < caps.mod_counts.count ? caps.mod_counts.data[i] : 0;
        int pref = -1;
        for (int k = 0; k < 4; ++k)
            if (kFormats[k] == f) pref = 4 - k;
        for (uint32_t j = 0; j < n; ++j, ++at) {
            if (pref < 0 || at >= caps.modifiers.count) continue;
            uint64_t m = caps.modifiers.data[at];
            if (!takes(f, m)) continue;
            int score = pref * 2 + (m != kLinear ? 1 : 0);
            if (score > best) {
                best = score;
                best_f = f;
                best_m = m;
                planes = at < caps.plane_counts.count ? caps.plane_counts.data[at] : 1;
            }
        }
    }
    if (best < 0) {
        failed("El fondo web no tiene un formato que niri pueda mostrar");
        return;
    }
    ww_evt_in_negotiate_buffers_t m{};
    m.directive.format = {best_f, best_m, planes};
    m.directive.sync_mode = WW_SYNC_SYNCOBJ_TIMELINE;
    m.directive.color = WW_COLOR_ENC_SRGB | WW_COLOR_RANGE_FULL | WW_COLOR_ALPHA_PREMUL;
    m.directive.mem_hint = caps.mem_hints & WW_MEM_HINT_DEVICE_LOCAL;
    m.directive.count = 3;
    m.directive.path = best_m == kLinear ? WAYWALLEN_BUFFER_PATH_COMPAT_LINEAR : WAYWALLEN_BUFFER_PATH_OPTIMIZED_SAME_DEVICE;
    m.directive.memory_source = best_m == kLinear ? WAYWALLEN_BUFFER_MEMORY_SOURCE_GPU_LINEAR : WAYWALLEN_BUFFER_MEMORY_SOURCE_GPU_NATIVE;
    send_msg(sock_, WW_EVT_IN_NEGOTIATE_BUFFERS, m, ww_evt_in_negotiate_buffers_encode);
}

// ── buffers ───────────────────────────────────────────────────────────────

void webwall_buffer_release(void* data, wl_buffer*) {
    auto* ref = static_cast<WebWall::BufRef*>(data);
    ref->self->released(ref->slot);
}
static const wl_buffer_listener buffer_listener = {.release = webwall_buffer_release};

void webwall_params_created(void* data, zwp_linux_buffer_params_v1* params, wl_buffer* buf) {
    auto* p = static_cast<Pending*>(data);
    WebWall* self = p->self;
    zwp_linux_buffer_params_v1_destroy(params);
    if (p->slot < static_cast<int>(self->slots_.size()) && p->out < static_cast<int>(self->slots_[p->slot].wl.size())) {
        self->refs_.push_back(std::make_unique<WebWall::BufRef>(WebWall::BufRef{self, p->slot}));
        wl_buffer_add_listener(buf, &buffer_listener, self->refs_.back().get());
        self->slots_[p->slot].wl[p->out] = buf;
    } else {
        wl_buffer_destroy(buf);
    }
    --self->pending_buffers_;
    delete p;
}

void webwall_params_failed(void* data, zwp_linux_buffer_params_v1* params) {
    auto* p = static_cast<Pending*>(data);
    WebWall* self = p->self;
    zwp_linux_buffer_params_v1_destroy(params);
    --self->pending_buffers_;
    delete p;
    Loop::get().post([self] { self->failed("niri no aceptó los fotogramas del fondo web"); });
}
static const zwp_linux_buffer_params_v1_listener params_listener = {.created = webwall_params_created,
                                                                    .failed = webwall_params_failed};

void WebWall::bind(const void* pool_v, const int* fds, size_t nfds) {
    const auto& pool = *static_cast<const waywallen_buffer_pool_t*>(pool_v);
    free_pool();
    uint32_t planes = pool.format.plane_count;
    if (nfds < static_cast<size_t>(pool.count) * planes || pool.stride.count < pool.count * planes) {
        failed("El fondo web mandó un búfer incompleto");
        return;
    }
    buf_w_ = static_cast<int>(pool.extent.width);
    buf_h_ = static_cast<int>(pool.extent.height);
    slots_.assign(pool.count, {});
    for (auto& s : slots_) s.wl.assign(outs_.size(), nullptr);
    uint64_t mod = pool.format.modifier;
    for (uint32_t i = 0; i < pool.count; ++i)
        for (size_t o = 0; o < outs_.size(); ++o) {
            zwp_linux_buffer_params_v1* params = zwp_linux_dmabuf_v1_create_params(wl_.dmabuf_);
            for (uint32_t p = 0; p < planes; ++p) {
                size_t k = i * planes + p;
                zwp_linux_buffer_params_v1_add(params, fds[k], p, pool.plane_offset.count > k ? pool.plane_offset.data[k] : 0,
                                               pool.stride.data[k], static_cast<uint32_t>(mod >> 32),
                                               static_cast<uint32_t>(mod & 0xffffffff));
            }
            zwp_linux_buffer_params_v1_add_listener(params, &params_listener, new Pending{this, static_cast<int>(i), static_cast<int>(o)});
            zwp_linux_buffer_params_v1_create(params, buf_w_, buf_h_, pool.format.fourcc, 0);
            ++pending_buffers_;
        }
    wl_display_flush(wl_.display_);  // (the fds were sent: ours can close, the caller does)
    for (auto& o : outs_) fit(o);
    send_control(paused_ ? WW_EVT_IN_PAUSE : WW_EVT_IN_PLAY);
    if (muted_) send_control(WW_EVT_IN_MUTE);
}

void WebWall::free_pool() {
    for (auto& s : slots_)
        for (wl_buffer* b : s.wl)
            if (b) wl_buffer_destroy(b);
    slots_.clear();
    refs_.clear();
    for (auto& o : outs_) o.shown = -1;
}

// ── frames ────────────────────────────────────────────────────────────────

void WebWall::frame_ready(uint32_t index, uint64_t point, int fence) {
    points_.push_back({point, false});
    if (index >= slots_.size()) {
        if (fence >= 0) close(fence);
        done(point);
        return;
    }
    drop_wait();  // a newer frame: the waiting one is skipped
    if (fence < 0) {
        present(static_cast<int>(index), point);
        return;
    }
    wait_fd_ = fence;
    wait_index_ = static_cast<int>(index);
    wait_point_ = point;
    Loop::get().watch(fence, POLLIN, [this](short) {
        int idx = wait_index_;
        uint64_t pt = wait_point_;
        Loop::get().unwatch(wait_fd_);
        close(wait_fd_);
        wait_fd_ = -1;
        present(idx, pt);
    });
}

void WebWall::drop_wait() {
    if (wait_fd_ < 0) return;
    Loop::get().unwatch(wait_fd_);
    close(wait_fd_);
    wait_fd_ = -1;
    done(wait_point_);
}

void WebWall::present(int index, uint64_t point) {
    Slot& s = slots_[index];
    int shown = 0;
    for (size_t o = 0; o < outs_.size(); ++o) {
        Out& out = outs_[o];
        if (!out.surf || !out.surf->configured() || o >= s.wl.size() || !s.wl[o]) continue;
        wl_surface* surf = out.surf->surface();
        wl_surface_attach(surf, s.wl[o], 0, 0);
        wl_surface_damage_buffer(surf, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surf);
        out.shown = index;
        ++shown;
    }
    if (shown == 0) {  // nowhere to show it yet: it goes straight back
        done(point);
        return;
    }
    s.busy += shown;
    s.release_point = point;
    s.pending = true;
    wl_display_flush(wl_.display_);
    if (!shown_) {
        shown_ = true;
        if (on_shown) on_shown();
    }
}

void WebWall::released(int index) {
    if (index < 0 || index >= static_cast<int>(slots_.size())) return;
    Slot& s = slots_[index];
    if (s.busy > 0 && --s.busy == 0 && s.pending) {
        s.pending = false;
        done(s.release_point);
    }
}

void WebWall::done(uint64_t point) {
    for (auto& p : points_)
        if (p.first == point) p.second = true;
    // move the timeline to the newest point with everything before it done
    uint64_t upto = 0;
    while (!points_.empty() && points_.front().second) {
        upto = points_.front().first;
        points_.erase(points_.begin());
    }
    if (!upto || drm_fd_ < 0 || !release_handle_) return;
    uint32_t handle = release_handle_;
    uint64_t pt = upto;
    drm_syncobj_timeline_array a{};
    a.handles = reinterpret_cast<uintptr_t>(&handle);
    a.points = reinterpret_cast<uintptr_t>(&pt);
    a.count_handles = 1;
    ioctl(drm_fd_, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &a);
}

// ── surfaces ──────────────────────────────────────────────────────────────

void WebWall::ensure_surfaces() {
    std::vector<wl_output*> want = wl_.outputs();
    if (want.empty()) want.push_back(nullptr);
    for (wl_output* out : want) {
        LayerSpec spec;
        spec.layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
        spec.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        spec.exclusive_zone = -1;
        spec.keyboard = KeyboardMode::None;
        spec.name = "niri-island-web";
        spec.output = out;
        Out o;
        o.output = out;
        o.surf = wl_.create_layer(spec);
        o.viewport = wp_viewporter_get_viewport(wl_.viewporter_, o.surf->surface());
        LayerSurface* raw = o.surf.get();
        raw->on_configure = [this, raw] {
            for (auto& x : outs_)
                if (x.surf.get() == raw) fit(x);
        };
        outs_.push_back(std::move(o));
    }
}

// The frame covers the screen, cropped to its middle.
void WebWall::fit(Out& o) {
    if (!o.viewport || !o.surf || !o.surf->configured()) return;
    int w = o.surf->width(), h = o.surf->height();
    wp_viewport_set_destination(o.viewport, w, h);
    if (buf_w_ > 0 && buf_h_ > 0 && w > 0 && h > 0) {
        double s = std::max(static_cast<double>(w) / buf_w_, static_cast<double>(h) / buf_h_);
        double sw = w / s, sh = h / s;
        wp_viewport_set_source(o.viewport, wl_fixed_from_double((buf_w_ - sw) / 2), wl_fixed_from_double((buf_h_ - sh) / 2),
                               wl_fixed_from_double(sw), wl_fixed_from_double(sh));
    }
    o.surf->commit();
}

}  // namespace island
