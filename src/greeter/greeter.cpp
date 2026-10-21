#include "greeter/greeter.hpp"

#include <cmath>
#include <dirent.h>
#include <pwd.h>
#include <thread>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"
#include "greeter/greetd.hpp"
#include "services/images.hpp"
#include "suite/lock.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace island {

using namespace paint;

namespace {

// Where the last user and session are kept: the greeter's own home (greetd
// runs it as its user), or the cache of whoever runs the demo.
std::string state_file() {
    std::string base = home_dir();
    if (base.empty() || access(base.c_str(), W_OK) != 0) base = "/tmp";
    return base + "/.cache/niri-island-greeter.state";
}

std::string desktop_value(const std::string& text, const std::string& key) {
    for (auto& line : split(text, '\n'))
        if (starts_with(line, key + "=")) return line.substr(key.size() + 1);
    return "";
}

}  // namespace

Greeter::Greeter(Wayland& wl, Options opt) : Overlay(wl), opt_(std::move(opt)) {
    char h[256] = {0};
    if (gethostname(h, sizeof(h) - 1) == 0) host_ = h;
}

Greeter::~Greeter() {
    if (bg_) cairo_surface_destroy(bg_);
    for (auto& u : users_)
        if (u.avatar) cairo_surface_destroy(u.avatar);
}

void Greeter::start() {
    load_users();
    load_sessions();
    load_state();
    // the wallpaper and the avatars: off the main thread
    auto bg = std::make_shared<cairo_surface_t*>(nullptr);
    auto avatars = std::make_shared<std::vector<cairo_surface_t*>>(users_.size(), nullptr);
    std::vector<std::string> homes, logins;
    for (auto& u : users_) {
        homes.push_back(u.home);
        logins.push_back(u.login);
    }
    std::string wp = opt_.wallpaper;
    Loop::get().async(
        [wp, bg, avatars, homes, logins] {
            if (!wp.empty())
                if (cairo_surface_t* img = load_image_blocking(wp, 480)) {
                    blur_argb(img, 7);
                    *bg = img;
                }
            for (size_t i = 0; i < homes.size(); ++i)
                for (std::string f : {"/var/lib/AccountsService/icons/" + logins[i], homes[i] + "/.face", homes[i] + "/.face.icon"})
                    if (file_exists(f) && access(f.c_str(), R_OK) == 0)
                        if (((*avatars)[i] = load_image_blocking(f, 96))) break;
        },
        [this, bg, avatars] {
            bg_ = *bg;
            for (auto& e : extras_) e.dirty = true;
            for (size_t i = 0; i < users_.size() && i < avatars->size(); ++i) users_[i].avatar = (*avatars)[i];
            request();
        });
    tick_ = Loop::get().after(0.5, [this] { request(); }, 0.5);  // clock and caret
    open();
    const auto& outs = wl_.outputs();
    for (size_t i = 1; i < outs.size(); ++i) {
        LayerSpec spec;
        spec.layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
        spec.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        spec.exclusive_zone = -1;
        spec.name = "niri-island-greeter-screen";
        spec.output = outs[i];
        Extra e;
        e.surf = wl_.create_layer(spec);
        LayerSurface* raw = e.surf.get();
        raw->on_configure = [this, raw] {
            for (auto& x : extras_)
                if (x.surf.get() == raw) x.dirty = true;
        };
        extras_.push_back(std::move(e));
    }
}

// People who can log in: real accounts with a real shell.
void Greeter::load_users() {
    setpwent();
    while (passwd* pw = getpwent()) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000) continue;
        std::string shell = pw->pw_shell ? pw->pw_shell : "";
        if (shell.find("nologin") != std::string::npos || shell.find("false") != std::string::npos) continue;
        User u;
        u.login = pw->pw_name;
        std::string gecos = pw->pw_gecos ? pw->pw_gecos : "";
        u.name = gecos.substr(0, gecos.find(','));
        if (u.name.empty()) u.name = u.login;
        u.home = pw->pw_dir ? pw->pw_dir : "";
        users_.push_back(std::move(u));
    }
    endpwent();
    if (users_.empty()) users_.push_back({"root", "root", "/root", nullptr});
}

// Wayland sessions from their .desktop files (niri first).
void Greeter::load_sessions() {
    for (const char* dir : {"/usr/share/wayland-sessions", "/usr/local/share/wayland-sessions"}) {
        DIR* d = opendir(dir);
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name;
            if (f.size() < 9 || f.substr(f.size() - 8) != ".desktop") continue;
            std::string txt;
            if (!read_file(std::string(dir) + "/" + f, txt)) continue;
            Session s{desktop_value(txt, "Name"), desktop_value(txt, "Exec"), desktop_value(txt, "DesktopNames")};
            if (s.exec.empty() || desktop_value(txt, "Hidden") == "true") continue;
            if (s.name.empty()) s.name = f.substr(0, f.size() - 8);
            sessions_.push_back(std::move(s));
        }
        closedir(d);
    }
    std::stable_sort(sessions_.begin(), sessions_.end(),
                     [](const Session& a, const Session& b) { return lower(a.name).find("niri") == 0 && lower(b.name).find("niri") != 0; });
    if (sessions_.empty()) sessions_.push_back({"niri", "niri-session", "niri"});
    sessions_.push_back({"Terminal", "/bin/sh -l", ""});  // a way in when the desktop is broken
}

void Greeter::load_state() {
    std::string txt;
    if (!read_file(state_file(), txt)) return;
    auto lines = split(txt, '\n');
    for (size_t i = 0; i < users_.size(); ++i)
        if (lines.size() > 0 && users_[i].login == lines[0]) user_ = static_cast<int>(i);
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (lines.size() > 1 && sessions_[i].name == lines[1]) session_ = static_cast<int>(i);
}

void Greeter::save_state() const {
    std::string f = state_file();
    make_dirs(f.substr(0, f.rfind('/')));
    write_file_atomic(f, users_[user_].login + "\n" + sessions_[session_].name + "\n");
}

void Greeter::message(const std::string& m) {
    info_ = m;
    msg_at_ = now_mono();
    request();
}

void Greeter::power(const char* what) {
    if (opt_.demo) {
        message(std::string("Vista previa: aquí se haría «") + what + "»");
        return;
    }
    proc::spawn({"systemctl", what});
}

void Greeter::submit() {
    if (checking_ || pass_.empty()) return;
    checking_ = true;
    error_.clear();
    std::string user = users_[user_].login, password = pass_;
    std::fill(pass_.begin(), pass_.end(), '\0');
    pass_.clear();
    // the session's command line, split like a shell would for simple cases
    std::vector<std::string> cmd = {"/bin/sh", "-c", "exec " + sessions_[session_].exec};
    const Session& se = sessions_[session_];
    std::vector<std::string> env = {"XDG_SESSION_TYPE=wayland", "XDG_SESSION_DESKTOP=" + lower(se.name)};
    std::string desk = se.desktop.empty() ? lower(se.name) : replace_all(se.desktop, ";", ":");
    if (!desk.empty() && desk.back() == ':') desk.pop_back();
    if (se.exec.find("/bin/sh") != 0) env.push_back("XDG_CURRENT_DESKTOP=" + desk);
    bool demo = opt_.demo;
    std::string sock = opt_.socket;
    std::thread([this, demo, sock, user, password, cmd, env]() mutable {
        std::string err;
        if (demo) {
            usleep(700000);
            err = password == "demo" ? "" : "Incorrecta · aquí es «demo»";
        } else {
            err = greetd_login(sock, user, password, cmd, env);
        }
        std::fill(password.begin(), password.end(), '\0');
        Loop::get().post([this, err] {
            checking_ = false;
            if (err.empty()) {
                save_state();
                if (on_done) on_done();
                return;
            }
            error_ = err;
            fail_at_ = now_mono();
            request();
        });
    }).detach();
    request();
}

void Greeter::key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    if (checking_) return;
    last_key_ = now_mono();
    bool ctrl = mods & Wayland::MOD_CTRL;
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) submit();
    else if (sym == XKB_KEY_BackSpace) {
        if (ctrl) pass_.clear();
        else if (!pass_.empty()) pass_.erase(utf8_prev(pass_, pass_.size()));
    } else if (sym == XKB_KEY_Escape) {
        if (pass_.empty() && opt_.demo && on_done) on_done();
        pass_.clear();
    } else if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) pass_.clear();
    else if (sym == XKB_KEY_F2 || (sym == XKB_KEY_Tab && pass_.empty())) {
        session_ = (session_ + 1) % static_cast<int>(sessions_.size());
    } else if ((sym == XKB_KEY_Up || sym == XKB_KEY_Down) && pass_.empty() && users_.size() > 1) {
        int n = static_cast<int>(users_.size());
        user_ = (user_ + (sym == XKB_KEY_Down ? 1 : n - 1)) % n;
        error_.clear();
    } else if (!utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20 && !ctrl) {
        if (pass_.size() < 512) pass_ += utf8;
        error_.clear();
    }
    request();
}

bool Greeter::animating() const {
    double now = now_mono();
    return checking_ || now - fail_at_ < 0.5;
}

// The wallpaper, blurred and darkened (the login screen and the others).
void Greeter::paint_backdrop(cairo_t* cr, float W, float H) const {
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
    cairo_set_source_rgba(cr, 0, 0, 0, 0.30);
    cairo_paint(cr);
}

bool Greeter::extras_want_frame() const {
    for (auto& e : extras_)
        if (e.dirty && e.surf->configured() && !e.surf->frame_pending()) return true;
    return false;
}

void Greeter::pump_extras() {
    for (auto& e : extras_) {
        if (!e.dirty || !e.surf->configured() || e.surf->frame_pending()) continue;
        cairo_t* cr = e.surf->begin_frame();
        if (!cr) continue;
        e.dirty = false;
        paint_backdrop(cr, static_cast<float>(e.surf->width()), static_cast<float>(e.surf->height()));
        e.surf->end_frame({});
    }
}

void Greeter::draw(cairo_t* cr, float W, float H, float) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    double now = now_mono();

    paint_backdrop(cr, W, H);

    // the docked island: the machine's name, a spinner while checking
    std::string label = host_.empty() ? "niri" : host_;
    float iw = std::max(110.0f, measure(label, {ui(12, "SemiBold")}).w + 52);
    Rect isl{std::round(W / 2 - iw / 2), 0, std::round(iw), 30};
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
    if (checking_) {
        float a = static_cast<float>(std::fmod(now * 5.0, 2 * M_PI));
        cairo_arc(cr, isl.x + 20, isl.cy() - 1, 7, a, a + 1.6);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.85);
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
    } else {
        glyph(cr, isl.x + 20, isl.cy() - 1, "󰍹", 12, Color{1, 1, 1, 0.85f});
    }
    text(cr, isl.x + 34, isl.cy() - 9, label, {ui(12, "SemiBold"), Color{1, 1, 1, 0.92f}, Align::Left, iw - 44});

    // date and the big two-line clock
    std::string date = fmt_time("%A, %e de %B");
    if (!date.empty()) date[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(date[0])));
    date = replace_all(date, "  ", " ");
    float big = std::round(H * 0.17f);
    float cy = H * 0.17f;
    text(cr, W / 2, cy, date, {ui(std::max(14.0f, H * 0.016f), "SemiBold"), Color{1, 1, 1, 0.92f}, Align::Center});
    text(cr, W / 2, cy + H * 0.022f, fmt_time("%H"), {ui(big, "Bold"), Color{1, 1, 1, 0.97f}, Align::Center});
    text(cr, W / 2, cy + H * 0.022f + big * 0.86f, fmt_time("%M"), {ui(big, "Bold"), Color{1, 1, 1, 0.42f}, Align::Center});

    // the user's capsule: switch user with the arrows either side
    const User& u = users_[user_];
    float shake = 0;
    float since_fail = static_cast<float>(now - fail_at_);
    if (since_fail < 0.45f) shake = std::sin(since_fail * 48) * 12 * (1 - since_fail / 0.45f);
    Rect cap{std::round(W / 2 - 170 + shake), std::round(H * 0.80f), 340, 56};
    fill(cr, cap, 28, Color{0.03f, 0.03f, 0.04f, 0.80f});
    Rect av{cap.x + 9, cap.y + 9, 38, 38};
    if (u.avatar) image(cr, u.avatar, av, 19);
    else {
        fill(cr, av, 19, p.accent);
        std::string ini = u.name.empty() ? "?" : u.name.substr(0, 1);
        ini[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(ini[0])));
        text(cr, av.cx(), av.cy() - 10, ini, {ui(16, "Bold"), p.on_accent, Align::Center});
    }
    float tx = cap.x + 58, tw = cap.w - 58 - (users_.size() > 1 ? 70 : 16);
    text(cr, tx, cap.y + 9, u.name, {ui(13.5f, "Bold"), Color{1, 1, 1, 0.95f}, Align::Left, tw});
    std::string sub;
    Color subc{1, 1, 1, 0.6f};
    if (checking_) sub = "Iniciando sesión…";
    else if (!error_.empty() && pass_.empty()) {
        sub = error_;
        subc = Color{1, 0.72f, 0.72f, 0.95f};
    } else if (pass_.empty()) sub = "Escribe tu contraseña";
    if (!sub.empty()) text(cr, tx, cap.y + 29, sub, {ui(11.5f), subc, Align::Left, tw});
    else {
        size_t n = std::min<size_t>(utf8_length(pass_), 22);
        for (size_t i = 0; i < n; ++i) fill(cr, {tx + 1 + i * 10.0f, cap.y + 35, 6, 6}, 3, Color{1, 1, 1, 0.9f});
        if (std::fmod(now - last_key_, 1.0) < 0.55) fill(cr, {tx + 1 + n * 10.0f, cap.y + 32, 1.5f, 12}, 0, p.accent);
    }
    if (users_.size() > 1) {
        int n = static_cast<int>(users_.size());
        w::icon_button(c, ID("gr.uprev"), {cap.right() - 68, cap.y + 13, 30, 30}, "󰅁", 14).click([this, n] {
            user_ = (user_ + n - 1) % n;
            error_.clear();
        });
        w::icon_button(c, ID("gr.unext"), {cap.right() - 38, cap.y + 13, 30, 30}, "󰅂", 14).click([this, n] {
            user_ = (user_ + 1) % n;
            error_.clear();
        });
    }

    // the session, under the capsule: click (or F2) for the next one
    std::string sess = sessions_[session_].name;
    float sw = measure(sess, {ui(12, "SemiBold")}).w + 62;
    Rect sp{std::round(W / 2 - sw / 2), cap.bottom() + 14, std::round(sw), 30};
    uint64_t sid = ID("gr.session");
    float hv = ui_.hover(sid);
    fill(cr, sp, 15, Color{0.03f, 0.03f, 0.04f, 0.55f + 0.2f * hv});
    glyph(cr, sp.x + 18, sp.cy(), "󰍹", 13, Color{1, 1, 1, 0.8f});
    text(cr, sp.x + 32, sp.cy() - 8, sess, {ui(12, "SemiBold"), Color{1, 1, 1, 0.88f}, Align::Left});
    glyph(cr, sp.right() - 14, sp.cy(), "󰅀", 12, Color{1, 1, 1, 0.6f});
    ui_.add(sid, sp).click([this] { session_ = (session_ + 1) % static_cast<int>(sessions_.size()); });

    // power, bottom right
    struct Pw {
        const char* glyph;
        const char* what;
        const char* tip;
    };
    const Pw pw[3] = {{"󰤄", "suspend", "Suspender"}, {"󰜉", "reboot", "Reiniciar"}, {"󰐥", "poweroff", "Apagar"}};
    for (int i = 0; i < 3; ++i) {
        Rect r{W - 24 - (3 - i) * 46.0f, H - 24 - 40, 40, 40};
        uint64_t id = ID("gr.pw", i);
        float h = ui_.hover(id);
        fill(cr, r, 20, Color{0.03f, 0.03f, 0.04f, 0.55f + 0.25f * h});
        glyph(cr, r.cx(), r.cy(), pw[i].glyph, 16, Color{1, 1, 1, 0.75f + 0.25f * h});
        if (h > 0.05f) text(cr, r.cx(), r.y - 22, pw[i].tip, {ui(11), Color{1, 1, 1, 0.8f * h}, Align::Center});
        const char* what = pw[i].what;
        ui_.add(id, r).click([this, what] { power(what); });
    }

    // the hints, bottom left; and the demo's badge
    text(cr, 24, H - 24 - 30, users_.size() > 1 ? "↑ ↓ usuario · F2 sesión · Enter entrar" : "F2 sesión · Enter entrar",
         {ui(11), Color{1, 1, 1, 0.45f}, Align::Left});
    if (opt_.demo) {
        Rect b{20, 20, 330, 30};
        fill(cr, b, 15, p.accent.with_alpha(0.9f));
        text(cr, b.cx(), b.cy() - 8, "Vista previa · contraseña «demo» · Esc para salir", {ui(11.5f, "SemiBold"), p.on_accent, Align::Center});
    }
    if (now - msg_at_ < 3) {
        float a = static_cast<float>(std::min(1.0, (3 - (now - msg_at_)) * 2));
        float mw = measure(info_, {ui(12)}).w + 32;
        Rect m{W / 2 - mw / 2, sp.bottom() + 14, mw, 30};
        fill(cr, m, 15, Color{0.03f, 0.03f, 0.04f, 0.8f * a});
        text(cr, m.cx(), m.cy() - 8, info_, {ui(12), Color{1, 1, 1, 0.9f * a}, Align::Center});
    }
}

}  // namespace island
