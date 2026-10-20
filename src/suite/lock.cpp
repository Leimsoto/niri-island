#include "suite/lock.hpp"

#include <cmath>
#include <cstring>
#include <pwd.h>
#include <security/pam_appl.h>
#include <thread>
#include <unistd.h>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/util.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

namespace {

// ── PAM, on a worker thread ──
struct PamData {
    std::string password;
};

int pam_conv_fn(int n, const pam_message** msg, pam_response** resp, void* ud) {
    auto* d = static_cast<PamData*>(ud);
    auto* r = static_cast<pam_response*>(calloc(static_cast<size_t>(n), sizeof(pam_response)));
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; ++i)
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
            r[i].resp = strdup(d->password.c_str());
    *resp = r;
    return PAM_SUCCESS;
}

bool pam_check(const std::string& user, const std::string& password) {
    PamData d{password};
    pam_conv conv{pam_conv_fn, &d};
    pam_handle_t* h = nullptr;
    // our own service file if installed, else the one swaylock uses, else login
    const char* service = file_exists("/etc/pam.d/niri-island") ? "niri-island"
                        : file_exists("/etc/pam.d/swaylock")    ? "swaylock"
                                                                : "login";
    if (pam_start(service, user.c_str(), &conv, &h) != PAM_SUCCESS) return false;
    int r = pam_authenticate(h, 0);
    if (r == PAM_SUCCESS) r = pam_acct_mgmt(h, 0);
    pam_end(h, r);
    std::fill(d.password.begin(), d.password.end(), '\0');
    return r == PAM_SUCCESS;
}

// Box blur of an ARGB32 surface, three passes per direction.
void blur_argb(cairo_surface_t* s, int radius) {
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char* px = cairo_image_surface_get_data(s);
    std::vector<unsigned char> tmp(static_cast<size_t>(std::max(w, h)) * 4);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            unsigned char* row = px + y * stride;
            for (int c = 0; c < 4; ++c) {
                int sum = 0, n = 2 * radius + 1;
                for (int k = -radius; k <= radius; ++k) sum += row[std::clamp(k, 0, w - 1) * 4 + c];
                for (int x = 0; x < w; ++x) {
                    tmp[x * 4 + c] = static_cast<unsigned char>(sum / n);
                    sum += row[std::min(x + radius + 1, w - 1) * 4 + c] - row[std::max(x - radius, 0) * 4 + c];
                }
            }
            memcpy(row, tmp.data(), static_cast<size_t>(w) * 4);
        }
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) {
                int sum = 0, n = 2 * radius + 1;
                for (int k = -radius; k <= radius; ++k) sum += px[std::clamp(k, 0, h - 1) * stride + x * 4 + c];
                for (int y = 0; y < h; ++y) {
                    tmp[y * 4 + c] = static_cast<unsigned char>(sum / n);
                    sum += px[std::min(y + radius + 1, h - 1) * stride + x * 4 + c] - px[std::max(y - radius, 0) * stride + x * 4 + c];
                }
            }
            for (int y = 0; y < h; ++y) memcpy(px + y * stride + x * 4, &tmp[y * 4], 4);
        }
    }
    cairo_surface_mark_dirty(s);
}

}  // namespace

static const ext_session_lock_v1_listener kListener = {.locked = Lock::on_locked_cb, .finished = Lock::on_finished_cb};

Lock::~Lock() {
    if (bg_) cairo_surface_destroy(bg_);
    if (avatar_) cairo_surface_destroy(avatar_);
}

bool Lock::lock(double preview) {
    if (state_ != State::Idle) return true;
    if (!wl_.can_lock() || wl_.outputs().empty()) return false;
    lock_ = ext_session_lock_manager_v1_lock(wl_.lock_mgr_);
    ext_session_lock_v1_add_listener(lock_, &kListener, this);
    state_ = State::Locking;
    pass_.clear();
    checking_ = false;
    fail_at_ = -10;
    opened_at_ = now_mono();
    // who we are
    if (passwd* pw = getpwuid(getuid())) {
        user_ = pw->pw_name ? pw->pw_name : "";
        std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
        name_ = gecos.substr(0, gecos.find(','));
        if (name_.empty()) name_ = user_;
    }
    for (wl_output* o : wl_.outputs()) {
        Out out;
        out.surf = wl_.create_lock_surface(lock_, o);
        LayerSurface* s = out.surf.get();
        s->on_configure = [this] { request(); };
        s->on_key = [this](xkb_keysym_t sym, const std::string& utf8, uint32_t mods) { key(sym, utf8, mods); };
        outs_.push_back(std::move(out));
    }
    // the clock ticks; the caret blinks
    tick_ = Loop::get().after(0.5, [this] { request(); }, 0.5);
    // the wallpaper, small and blurred, and the avatar: off the main thread
    std::string wp = wallpaper;
    std::string home = home_dir(), user = user_;
    auto bg = std::make_shared<cairo_surface_t*>(nullptr);
    auto av = std::make_shared<cairo_surface_t*>(nullptr);
    Loop::get().async(
        [wp, home, user, bg, av] {
            if (!wp.empty())
                if (cairo_surface_t* img = load_image_blocking(wp, 480)) {
                    blur_argb(img, 7);
                    *bg = img;
                }
            for (std::string f : {home + "/.face", home + "/.face.icon", "/var/lib/AccountsService/icons/" + user})
                if (file_exists(f))
                    if ((*av = load_image_blocking(f, 96))) break;
        },
        [this, bg, av] {
            if (bg_) cairo_surface_destroy(bg_);
            if (avatar_) cairo_surface_destroy(avatar_);
            bg_ = *bg;
            avatar_ = *av;
            request();
        });
    if (preview > 0) preview_timer_ = Loop::get().after(preview, [this] {
        preview_timer_ = 0;
        unlock();
    });
    wl_display_flush(wl_.display_);
    return true;
}

void Lock::on_locked_cb(void* data, ext_session_lock_v1*) {
    auto* self = static_cast<Lock*>(data);
    self->state_ = State::Locked;
    self->request();
}

void Lock::on_finished_cb(void* data, ext_session_lock_v1*) {
    // Refused (another locker), or the compositor ended it: tidy up.
    auto* self = static_cast<Lock*>(data);
    Loop::get().post([self] {
        bool was_locked = self->state_ == State::Locked;
        if (self->lock_) ext_session_lock_v1_destroy(self->lock_);
        self->lock_ = nullptr;
        self->outs_.clear();
        self->state_ = State::Idle;
        Loop::get().cancel(self->tick_);
        self->tick_ = 0;
        if (!was_locked && self->on_failed) self->on_failed();
    });
}

void Lock::unlock() {
    if (state_ == State::Idle) return;
    Loop::get().cancel(tick_);
    Loop::get().cancel(preview_timer_);
    tick_ = preview_timer_ = 0;
    if (lock_) ext_session_lock_v1_unlock_and_destroy(lock_);
    lock_ = nullptr;
    outs_.clear();
    state_ = State::Idle;
    std::fill(pass_.begin(), pass_.end(), '\0');
    pass_.clear();
    wl_display_flush(wl_.display_);
    if (on_unlocked) on_unlocked();
}

void Lock::request() {
    for (auto& o : outs_) o.dirty = true;
}

bool Lock::wants_frame() const {
    for (auto& o : outs_)
        if (o.dirty && o.surf && o.surf->configured() && !o.surf->frame_pending()) return true;
    return false;
}

void Lock::pump() {
    for (auto& o : outs_)
        if (o.dirty && o.surf && o.surf->configured() && !o.surf->frame_pending()) render(o);
}

void Lock::key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    if (checking_) return;
    last_key_ = now_mono();
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        submit();
    } else if (sym == XKB_KEY_BackSpace) {
        if (mods & Wayland::MOD_CTRL) pass_.clear();
        else if (!pass_.empty()) pass_.erase(utf8_prev(pass_, pass_.size()));
    } else if (sym == XKB_KEY_Escape || ((mods & Wayland::MOD_CTRL) && (sym == XKB_KEY_u || sym == XKB_KEY_U))) {
        pass_.clear();
    } else if (!utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20 && !(mods & Wayland::MOD_CTRL)) {
        if (pass_.size() < 512) pass_ += utf8;
    }
    request();
}

void Lock::submit() {
    if (pass_.empty() || checking_) return;
    checking_ = true;
    std::string user = user_, password = pass_;
    std::fill(pass_.begin(), pass_.end(), '\0');
    pass_.clear();
    // PAM can take a while (and sleeps on failure): off the main thread.
    std::thread([this, user, password]() mutable {
        bool ok = pam_check(user, password);
        std::fill(password.begin(), password.end(), '\0');
        Loop::get().post([this, ok] {
            checking_ = false;
            if (ok) {
                unlock();
                return;
            }
            fail_at_ = now_mono();
            request();
        });
    }).detach();
    request();
}

void Lock::render(Out& o) {
    LayerSurface* s = o.surf.get();
    cairo_t* cr = s->begin_frame();
    if (!cr) return;
    o.dirty = false;
    const auto& p = T();
    float W = static_cast<float>(s->width()), H = static_cast<float>(s->height());
    double now = now_mono();

    // the wallpaper, blurred, slightly darkened
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.08);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    if (bg_) {
        int iw = cairo_image_surface_get_width(bg_), ih = cairo_image_surface_get_height(bg_);
        double sc = std::max(W / iw, H / ih);
        cairo_save(cr);
        cairo_translate(cr, (W - iw * sc) / 2, (H - ih * sc) / 2);
        cairo_scale(cr, sc, sc);
        cairo_set_source_surface(cr, bg_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_set_source_rgba(cr, 0, 0, 0, 0.28);
    cairo_paint(cr);

    // a small island docked at the top, with the padlock (spinning while checking)
    Rect isl{std::round(W / 2 - 34), 0, 68, 30};
    cairo_new_path(cr);
    cairo_move_to(cr, isl.x - 10, 0);
    cairo_arc(cr, isl.x - 10, 10, 10, -M_PI / 2, 0);
    cairo_line_to(cr, isl.x, isl.bottom() - 14);
    cairo_arc_negative(cr, isl.x + 14, isl.bottom() - 14, 14, M_PI, M_PI / 2);
    cairo_line_to(cr, isl.right() - 14, isl.bottom());
    cairo_arc_negative(cr, isl.right() - 14, isl.bottom() - 14, 14, M_PI / 2, 0);
    cairo_line_to(cr, isl.right(), 10);
    cairo_arc(cr, isl.right() + 10, 10, 10, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.96);
    cairo_fill(cr);
    glyph(cr, isl.cx(), isl.cy() - 1, "󰌾", 13, Color{1, 1, 1, 0.92f});
    if (checking_) {
        float a = static_cast<float>(std::fmod(now * 5.0, 2 * M_PI));
        cairo_arc(cr, isl.cx(), isl.cy() - 1, 11, a, a + 1.6);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    }

    // date and the big two-line clock
    std::string date = fmt_time("%A, %e de %B");
    if (!date.empty()) date[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(date[0])));
    date = replace_all(date, "  ", " ");
    float big = std::round(H * 0.17f);
    float cy = H * 0.20f;
    text(cr, W / 2, cy, date, {ui(std::max(14.0f, H * 0.016f), "SemiBold"), Color{1, 1, 1, 0.92f}, Align::Center});
    text(cr, W / 2, cy + H * 0.022f, fmt_time(config().clock_24h ? "%H" : "%I"), {ui(big, "Bold"), Color{1, 1, 1, 0.97f}, Align::Center});
    text(cr, W / 2, cy + H * 0.022f + big * 0.86f, fmt_time("%M"), {ui(big, "Bold"), Color{1, 1, 1, 0.42f}, Align::Center});

    // the user's capsule: avatar, name, and the password (or what is going on)
    float shake = 0;
    float since_fail = static_cast<float>(now - fail_at_);
    if (since_fail < 0.45f) shake = std::sin(since_fail * 48) * 12 * (1 - since_fail / 0.45f);
    Rect cap{std::round(W / 2 - 150 + shake), std::round(H * 0.86f), 300, 48};
    fill(cr, cap, 24, Color{0.03f, 0.03f, 0.04f, 0.78f});
    Rect av{cap.x + 8, cap.y + 8, 32, 32};
    if (avatar_) image(cr, avatar_, av, 16);
    else {
        fill(cr, av, 16, p.accent);
        std::string ini = name_.empty() ? "?" : name_.substr(0, 1);
        text(cr, av.cx(), av.cy() - 9, ini, {ui(14, "Bold"), p.on_accent, Align::Center});
    }
    text(cr, cap.x + 50, cap.y + 7, name_, {ui(12.5f, "Bold"), Color{1, 1, 1, 0.95f}, Align::Left, cap.w - 64});
    std::string sub;
    Color subc{1, 1, 1, 0.6f};
    if (checking_) sub = "Comprobando…";
    else if (since_fail < 2.5f && pass_.empty()) {
        sub = "Contraseña incorrecta";
        subc = Color{1, 0.72f, 0.72f, 0.95f};
    } else if (pass_.empty()) sub = "Escribe tu contraseña";
    if (!sub.empty()) text(cr, cap.x + 50, cap.y + 25, sub, {ui(11), subc, Align::Left, cap.w - 64});
    else {
        // one dot per character (no length leak beyond what is on screen anyway)
        size_t n = std::min<size_t>(utf8_length(pass_), 22);
        for (size_t i = 0; i < n; ++i) fill(cr, {cap.x + 51 + i * 10.0f, cap.y + 30, 6, 6}, 3, Color{1, 1, 1, 0.9f});
        bool blink = std::fmod(now - last_key_, 1.0) < 0.55;
        if (blink) fill(cr, {cap.x + 51 + n * 10.0f, cap.y + 27, 1.5f, 12}, 0, p.accent);
    }
    s->end_frame({});
    if (since_fail < 0.5f || checking_) o.dirty = true;  // animating
}

}  // namespace island
