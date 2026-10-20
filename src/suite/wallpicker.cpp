#include "suite/wallpicker.hpp"

#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace island {

using namespace paint;
using json = nlohmann::json;

namespace {

bool is_image(const std::string& e) {
    return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "bmp" || e == "gif" || e == "avif";
}
bool is_video(const std::string& e) { return e == "mp4" || e == "webm" || e == "mkv" || e == "mov"; }

void walk(const std::string& dir, std::vector<Wallpaper>& out, int depth = 0) {
    if (depth > 5) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        std::string full = dir + "/" + n;
        struct stat st {};
        if (stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            walk(full, out, depth + 1);
            continue;
        }
        size_t dot = n.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = lower(n.substr(dot + 1));
        bool vid = is_video(ext);
        if (!vid && !is_image(ext)) continue;
        Wallpaper w;
        w.path = full;
        w.name = n.substr(0, dot);
        w.video = vid;
        w.mtime = static_cast<int64_t>(st.st_mtime);
        out.push_back(std::move(w));
    }
    closedir(d);
}

void rgb_to_hsv(Color c, float& h, float& s, float& v) {
    float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b});
    v = mx;
    float d = mx - mn;
    s = mx > 0 ? d / mx : 0;
    if (d <= 1e-5f) {
        h = 0;
        return;
    }
    if (mx == c.r) h = 60.0f * std::fmod((c.g - c.b) / d, 6.0f);
    else if (mx == c.g) h = 60.0f * ((c.b - c.r) / d + 2);
    else h = 60.0f * ((c.r - c.g) / d + 4);
    if (h < 0) h += 360;
}

Color hsv(float h, float s, float v) {
    float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    return {r + m, g + m, b + m, 1};
}

// Builds (or reuses) a thumbnail and measures its colour. Worker thread.
bool make_thumb(Wallpaper& w, const std::string& dir) {
    char name[40];
    snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash_str(w.path + std::to_string(w.mtime))));
    std::string jpg = dir + "/" + name + ".jpg", png = dir + "/" + name + ".png";
    std::string out = file_exists(jpg) ? jpg : file_exists(png) ? png : "";
    if (out.empty()) {
        if (w.video) {
            if (!proc::has("ffmpeg")) return false;
            proc::run({"ffmpeg", "-y", "-v", "error", "-ss", "1.5", "-i", w.path, "-frames:v", "1", "-vf",
                       "scale=520:-2", jpg},
                      20000);
            if (!file_exists(jpg))  // very short clips: first frame
                proc::run({"ffmpeg", "-y", "-v", "error", "-i", w.path, "-frames:v", "1", "-vf", "scale=520:-2", jpg},
                          20000);
            if (!file_exists(jpg)) return false;
            out = jpg;
        } else {
            GError* err = nullptr;
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(w.path.c_str(), 520, 520, TRUE, &err);
            if (!pb) {
                if (err) g_error_free(err);
                return false;
            }
            bool alpha = gdk_pixbuf_get_has_alpha(pb);
            out = alpha ? png : jpg;
            gboolean ok = alpha ? gdk_pixbuf_save(pb, out.c_str(), "png", nullptr, nullptr)
                                : gdk_pixbuf_save(pb, out.c_str(), "jpeg", nullptr, "quality", "86", nullptr);
            g_object_unref(pb);
            if (!ok) return false;
        }
    }
    w.thumb = out;
    if (w.hue < 0) {
        cairo_surface_t* s = load_image_blocking(out, 64);
        if (s) {
            Color c = dominant_color(s);
            cairo_surface_destroy(s);
            rgb_to_hsv(c, w.hue, w.sat, w.val);
        }
    }
    return true;
}

// Pointy-top hexagon.
void hex_path(cairo_t* cr, float cx, float cy, float r) {
    cairo_new_path(cr);
    for (int i = 0; i < 6; ++i) {
        double a = M_PI / 180.0 * (60 * i - 90);
        double x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}

float ease_out_back(float t) {
    t = clamp01(t);
    const float c1 = 1.4f, c3 = c1 + 1;
    float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}

}  // namespace

std::string WallPicker::current_wallpaper() const {
    std::string text;
    if (!read_file(expand_home(config().wallpaper_file), text) && !read_file(state_dir() + "/wallpaper.json", text)) return "";
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j.value("path", "") : "";
}

WallPicker::~WallPicker() { free_caches(); }

void WallPicker::show() {
    if (exists()) return;
    slices_ = config().wallpaper_picker_style != "grid";
    open();
}

void WallPicker::closed() {
    // keep the slices and lights (the next opening is instant); drop the big copy
    if (big_fit_) {
        cairo_surface_destroy(big_fit_);
        big_fit_ = nullptr;
    }
    big_path_.clear();
}

void WallPicker::opened() {
    opened_at_ = now_mono();
    open_t0_ = now_mono();
    entering_ = true;
    inserting_ = inserted_ = moved_ = leaving_ = left_ = false;
    if (remote_) {  // always open on the local folders
        remote_ = false;
        walls_ = std::move(local_walls_);
        local_walls_.clear();
    }
    downloading_ = false;
    backdrop_ready_ = false;
    applied_path_.clear();
    filter_t0_ = 0;
    kind_ = 0;
    hue_ = -1;
    current_ = current_wallpaper();
    search_.clear();
    scroll_.reset(0);
    if (walls_.empty()) scan();
    else {
        refilter();
        scan();  // refresh in the background: new files, new thumbs
    }
    // Start the selection on the wallpaper in use.
    for (size_t i = 0; i < visible_.size(); ++i)
        if (walls_[visible_[i]].path == current_) sel_ = static_cast<int>(i);
    pos_.reset(static_cast<float>(std::max(0, sel_)));
}

void WallPicker::scan() {
    if (scanning_) return;
    scanning_ = true;
    std::string thumbs = cache_dir() + "/wallthumbs";
    make_dirs(thumbs);
    std::string index_path = cache_dir() + "/wallpapers.json";
    auto found = std::make_shared<std::vector<Wallpaper>>();
    Loop::get().async(
        [found, index_path] {
            for (auto& d : config().wallpaper_dirs) walk(expand_home(d), *found);
            std::sort(found->begin(), found->end(), [](const Wallpaper& a, const Wallpaper& b) {
                if (a.video != b.video) return !a.video;
                return lower(a.name) < lower(b.name);
            });
            // Colours and thumbnails measured last time.
            std::string text;
            if (read_file(index_path, text)) {
                json j = json::parse(text, nullptr, false);
                if (j.is_object())
                    for (auto& w : *found) {
                        auto it = j.find(w.path);
                        if (it == j.end() || !it->is_object() || it->value("mtime", (int64_t)0) != w.mtime) continue;
                        w.thumb = it->value("thumb", "");
                        w.hue = it->value("hue", -1.0f);
                        w.sat = it->value("sat", 0.0f);
                        w.val = it->value("val", 0.0f);
                        if (!w.thumb.empty() && !file_exists(w.thumb)) w.thumb.clear();
                    }
            }
        },
        [this, found, thumbs, index_path] {
            // Keep thumbnails already known for files that did not change.
            std::unordered_map<std::string, Wallpaper*> old;
            for (auto& w : walls_) old[w.path] = &w;
            for (auto& w : *found) {
                auto it = old.find(w.path);
                if (it != old.end() && it->second->mtime == w.mtime && w.thumb.empty()) {
                    w.thumb = it->second->thumb;
                    w.hue = it->second->hue;
                    w.sat = it->second->sat;
                    w.val = it->second->val;
                }
            }
            if (remote_) {  // browsing wallhaven: the local list waits
                local_walls_ = std::move(*found);
                scanning_ = false;
                return;
            }
            walls_ = std::move(*found);
            refilter();
            for (size_t i = 0; i < visible_.size(); ++i)
                if (walls_[visible_[i]].path == current_ && (sel_ < 0 || (!moved_ && slices_))) sel_ = static_cast<int>(i);
            if (slices_ && !moved_) pos_.reset(static_cast<float>(std::max(0, sel_)));
            request();

            // Thumbnails still missing, in small batches so the grid fills in.
            auto todo = std::make_shared<std::vector<Wallpaper>>();
            for (auto& w : walls_)
                if (w.thumb.empty() || w.hue < 0) todo->push_back(w);
            thumbs_left_ = static_cast<int>(todo->size());
            if (todo->empty()) {
                scanning_ = false;
                return;
            }
            auto run_batch = std::make_shared<std::function<void(size_t)>>();
            *run_batch = [this, todo, thumbs, index_path, run_batch](size_t from) {
                auto batch = std::make_shared<std::vector<Wallpaper>>(
                    todo->begin() + from, todo->begin() + std::min(todo->size(), from + 6));
                Loop::get().async(
                    [batch, thumbs] {
                        for (auto& w : *batch) make_thumb(w, thumbs);
                    },
                    [this, batch, from, todo, index_path, run_batch] {
                        for (auto& b : *batch)
                            for (auto& w : walls_)
                                if (w.path == b.path) {
                                    w.thumb = b.thumb;
                                    w.hue = b.hue;
                                    w.sat = b.sat;
                                    w.val = b.val;
                                }
                        thumbs_left_ = std::max(0, thumbs_left_ - static_cast<int>(batch->size()));
                        request();
                        size_t next = from + batch->size();
                        if (next < todo->size()) {
                            (*run_batch)(next);
                            return;
                        }
                        // Done: remember everything for next time.
                        json j = json::object();
                        for (auto& w : walls_)
                            if (!w.thumb.empty())
                                j[w.path] = {{"mtime", w.mtime}, {"thumb", w.thumb}, {"hue", w.hue}, {"sat", w.sat},
                                             {"val", w.val}};
                        write_file_atomic(index_path, j.dump());
                        scanning_ = false;
                        refilter();
                        request();
                    });
            };
            (*run_batch)(0);
        });
}

void WallPicker::refilter() {
    std::string q = lower(search_);
    std::string keep = sel_ >= 0 && sel_ < static_cast<int>(visible_.size()) ? walls_[visible_[sel_]].path : "";
    visible_.clear();
    for (size_t i = 0; i < walls_.size(); ++i) {
        const auto& w = walls_[i];
        if (w.remote) {  // wallhaven filters on its side
            visible_.push_back(static_cast<int>(i));
            continue;
        }
        if (kind_ == 1 && w.video) continue;
        if (kind_ == 2 && !w.video) continue;
        if (!q.empty() && lower(w.name).find(q) == std::string::npos) continue;
        if (hue_ >= 0) {
            if (w.hue < 0) continue;
            if (hue_ == 12) {
                if (w.sat > 0.22f) continue;
            } else {
                float target = hue_ * 30.0f;
                float d = std::fabs(w.hue - target);
                d = std::min(d, 360 - d);
                if (w.sat < 0.18f || d > 22.0f) continue;
            }
        }
        visible_.push_back(static_cast<int>(i));
    }
    sel_ = -1;
    for (size_t i = 0; i < visible_.size(); ++i)
        if (walls_[visible_[i]].path == keep) sel_ = static_cast<int>(i);
    if (sel_ < 0 && !visible_.empty()) sel_ = 0;
}

void WallPicker::apply(int index, bool stay) {
    if (index < 0 || index >= static_cast<int>(walls_.size())) return;
    const Wallpaper& w = walls_[index];
    std::string cmd = config().cmd_wallpaper_apply;
    if ((cmd.empty() || cmd == "internal" || cmd.find("skwd") != std::string::npos) && on_apply) {
        on_apply(w.path);
    } else {
        if (cmd.find("{path}") == std::string::npos) cmd += " {path}";
        cmd = replace_all(cmd, "{path}", shell_quote(w.path));
        Loop::get().async([cmd] { proc::run_quiet({"/bin/sh", "-c", cmd}, nullptr, 20000); });
        backdrop_ready_ = true;  // an outside command: nothing to wait for
    }
    applied_path_ = w.path;
    current_ = w.path;
    if (!stay) close();
}

void WallPicker::ensure_visible(int pos) {
    if (pos < 0 || cols_ <= 0) return;
    float cy = grid_y0_ + (pos / cols_) * row_step_;  // row centre at scroll 0
    float top = top_clip_ + radius_ + 8, bottom = bottom_clip_ - radius_ - 30;
    if (cy - scroll_.target < top) scroll_.set(std::max(0.0f, cy - top));
    else if (cy - scroll_.target > bottom) scroll_.set(std::min(max_scroll_, cy - bottom));
}

void WallPicker::canvas_scroll(float dy) {
    if (slices_ && remote_) {
        float max = std::max(0.0f, grid_content_h() - grid_view_h_);
        grid_scroll_.set(std::clamp(grid_scroll_.target + dy * 90, 0.0f, max));
        if (grid_scroll_.target > max - grid_row_h_ * 2) wh_fetch(true);
        request();
        return;
    }
    if (slices_) {
        slices_move(dy > 0 ? 1 : -1);
        return;
    }
    scroll_.set(std::clamp(scroll_.target + dy * 90, 0.0f, max_scroll_));
}

bool WallPicker::animating() const {
    if (slices_)
        return inserting_ || leaving_ || wh_loading_ || remote_ || !pos_.settled() || now_mono() - open_t0_ < 1.3 ||
               (filter_t0_ > 0 && now_mono() - filter_t0_ < 0.4);
    return entering_ || !scroll_.settled();
}

void WallPicker::key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    if (slices_) {
        key_slices(sym, utf8);
        return;
    }
    int n = static_cast<int>(visible_.size());
    auto move = [&](int d) {
        if (n == 0) return;
        sel_ = std::clamp((sel_ < 0 ? 0 : sel_) + d, 0, n - 1);
        ensure_visible(sel_);
    };
    switch (sym) {
        case XKB_KEY_Escape:
            if (!search_.empty()) {
                search_.clear();
                refilter();
            } else {
                close();
            }
            return;
        case XKB_KEY_Left: move(-1); return;
        case XKB_KEY_Right: move(1); return;
        case XKB_KEY_Up: move(-cols_); return;
        case XKB_KEY_Down: move(cols_); return;
        case XKB_KEY_Home: move(-n); return;
        case XKB_KEY_End: move(n); return;
        case XKB_KEY_Page_Up: move(-cols_ * 3); return;
        case XKB_KEY_Page_Down: move(cols_ * 3); return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (sel_ >= 0 && sel_ < n) apply(visible_[sel_], (mods & Wayland::MOD_SHIFT) != 0);
            return;
        case XKB_KEY_Tab:
            kind_ = (kind_ + 1) % 3;
            refilter();
            return;
        case XKB_KEY_BackSpace:
            if (!search_.empty()) {
                search_.erase(utf8_prev(search_, search_.size()));
                refilter();
                scroll_.set(0);
            }
            return;
        default: break;
    }
    if ((mods & Wayland::MOD_CTRL) && (sym == XKB_KEY_r || sym == XKB_KEY_R)) {
        if (n > 0) {
            std::mt19937 rng(static_cast<unsigned>(now_mono() * 1000));
            apply(visible_[rng() % n], false);
        }
        return;
    }
    if (!(mods & Wayland::MOD_CTRL) && !utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20) {
        search_ += utf8;
        refilter();
        scroll_.set(0);
    }
}

void WallPicker::draw(cairo_t* cr, float w, float h, float dt) {
    if (slices_) {
        draw_slices(cr, w, h, dt);
        return;
    }
    const auto& p = T();
    w::Ctx c{cr, ui_};
    scroll_.update(dt);

    // Backdrop: the desktop shows through, darkened, with a vignette.
    cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.74);
    cairo_paint(cr);
    cairo_pattern_t* vg = cairo_pattern_create_radial(w / 2, h / 2, h * 0.25, w / 2, h / 2, w * 0.75);
    cairo_pattern_add_color_stop_rgba(vg, 0, 0, 0, 0, 0);
    cairo_pattern_add_color_stop_rgba(vg, 1, 0, 0, 0, 0.45);
    cairo_set_source(cr, vg);
    cairo_paint(cr);
    cairo_pattern_destroy(vg);
    // Anything not on a control closes nothing: the grid takes the whole screen.
    ui_.add(ID("wp.bg"), {0, 0, w, h}).cursor_shape(CursorShape::Default);

    // ── top bar ──────────────────────────────────────────────────────
    float bar_h = 52;
    float bw = std::min(w - 40, 1240.0f);
    Rect bar{(w - bw) / 2, 22, bw, bar_h};
    fill(cr, bar, bar_h / 2, p.island.with_alpha(0.94f));
    stroke(cr, bar, bar_h / 2, p.text.with_alpha(0.08f), 1);
    float x = bar.x + 10;
    const char* kinds[] = {"Todo", "Imágenes", "Vídeos"};
    for (int i = 0; i < 3; ++i) {
        float kw = measure(kinds[i], {ui(12.5f, "SemiBold")}).w + 28;
        w::pill(c, ID("wp.kind", i), {x, bar.y + 9, kw, 34}, kinds[i], nullptr, kind_ == i ? w::Style::Accent : w::Style::Ghost,
                12.5f)
            .click([this, i] {
                kind_ = i;
                refilter();
                scroll_.set(0);
            });
        x += kw + 4;
    }
    x += 10;
    divider(cr, x, bar.y + 14, x, bar.bottom() - 14, p.text.with_alpha(0.12f));
    x += 14;
    // Colour filter: hexagons in the hue they stand for, greys last.
    for (int i = 0; i <= 12; ++i) {
        uint64_t id = ID("wp.hue", i);
        float hv = ui_.hover(id);
        float r = 10 + hv * 2 + (hue_ == i ? 2 : 0);
        float cx = x + 12, cy = bar.cy();
        Color col = i == 12 ? Color{0.6f, 0.6f, 0.62f, 1} : hsv(i * 30.0f, 0.62f, 0.95f);
        hex_path(cr, cx, cy, r);
        set(cr, col.with_alpha(hue_ < 0 || hue_ == i ? 1.0f : 0.45f));
        cairo_fill_preserve(cr);
        if (hue_ == i) {
            set(cr, Color{1, 1, 1, 0.9f});
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
        } else {
            cairo_new_path(cr);
        }
        ui_.add(id, {x, bar.y + 8, 24, 36}).click([this, i] {
            hue_ = hue_ == i ? -1 : i;
            refilter();
            scroll_.set(0);
        });
        x += 26;
    }
    x += 10;
    divider(cr, x, bar.y + 14, x, bar.bottom() - 14, p.text.with_alpha(0.12f));
    x += 14;
    // Search (type anywhere)
    float right_btns = 3 * 40 + 8;
    Rect sr{x, bar.y + 9, std::max(120.0f, bar.right() - right_btns - x - 10), 34};
    fill(cr, sr, 17, p.surface_hi);
    glyph(cr, sr.x + 18, sr.cy(), "󰍉", 14, p.text_dim);
    if (search_.empty())
        text(cr, sr.x + 34, sr.cy() - 9, "Escribe para buscar…", {ui(12.5f), p.text_faint, Align::Left, sr.w - 44});
    else {
        Size ts = text(cr, sr.x + 34, sr.cy() - 9, search_, {ui(12.5f), p.text, Align::Left, sr.w - 44});
        if (std::fmod(now_mono(), 1.0) < 0.6) fill(cr, {sr.x + 35 + ts.w, sr.cy() - 8, 1.5f, 16}, 0.7f, p.accent);
    }
    float bx = bar.right() - right_btns;
    w::icon_button(c, ID("wp.random"), {bx, bar.y + 9, 34, 34}, "󰒝", 15).click([this] {
        if (visible_.empty()) return;
        std::mt19937 rng(static_cast<unsigned>(now_mono() * 1000));
        apply(visible_[rng() % visible_.size()], false);
    });
    w::icon_button(c, ID("wp.rescan"), {bx + 40, bar.y + 9, 34, 34}, "󰑓", 15, scanning_).click([this] { scan(); });
    w::icon_button(c, ID("wp.close"), {bx + 80, bar.y + 9, 34, 34}, "󰅖", 15).click([this] { close(); });

    // ── grid ─────────────────────────────────────────────────────────
    float R = std::clamp(w / 18.5f, 80.0f, 122.0f);
    float hw = std::sqrt(3.0f) * R, gap = 12;
    cols_ = std::max(1, static_cast<int>((w - 80 - (hw + gap) / 2) / (hw + gap)));
    float grid_w = cols_ * (hw + gap) + (hw + gap) / 2;
    float gx0 = (w - grid_w) / 2 + hw / 2;
    grid_y0_ = bar.bottom() + 40 + R;
    row_step_ = 1.5f * R + gap * 0.87f;
    int rows = (static_cast<int>(visible_.size()) + cols_ - 1) / cols_;
    content_h_ = rows * row_step_ + R;
    float top_clip = bar.bottom() + 14, bottom_clip = h - 54;
    radius_ = R;
    top_clip_ = top_clip;
    bottom_clip_ = bottom_clip;

    cairo_save(cr);
    cairo_rectangle(cr, 0, top_clip, w, bottom_clip - top_clip);
    cairo_clip(cr);
    ui_.push_clip({0, top_clip, w, bottom_clip - top_clip});

    double since = now_mono() - opened_at_;
    bool still_entering = false;
    int hot_pos = -1;
    float hot_cx = 0, hot_cy = 0, hot_s = 1;
    auto draw_tile = [&](int pos, float cx, float cy, float s, float a, bool lifted) {
        const Wallpaper& wp = walls_[visible_[pos]];
        float r = R * s;
        if (lifted) {
            // a soft glow under the lifted tile
            hex_path(cr, cx, cy + 6, r + 6);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * a);
            cairo_fill(cr);
        }
        cairo_save(cr);
        hex_path(cr, cx, cy, r);
        cairo_clip_preserve(cr);
        set(cr, p.surface_hi.with_alpha(a));
        cairo_fill(cr);
        cairo_surface_t* img = wp.thumb.empty() ? nullptr : ImageCache::get().request(wp.thumb, 420);
        if (img) image(cr, img, {cx - r, cy - r, 2 * r, 2 * r}, 0, a);
        else glyph(cr, cx, cy, wp.video ? "󰕧" : "󰋩", r * 0.28f, p.text_faint.fade(a));
        cairo_restore(cr);
        bool current = wp.path == current_;
        bool sel = pos == sel_;
        hex_path(cr, cx, cy, r - 1);
        if (current || sel || lifted) {
            set(cr, (current ? p.accent : Color{1, 1, 1, 1}).with_alpha((sel || lifted ? 0.95f : 0.8f) * a));
            cairo_set_line_width(cr, current ? 3.5 : 2.5);
        } else {
            set(cr, Color{0, 0, 0, 0.45f * a});
            cairo_set_line_width(cr, 1.5);
        }
        cairo_stroke(cr);
        if (wp.video) {
            Rect bd{cx - 14, cy + r * 0.52f, 28, 20};
            fill(cr, bd, 10, Color{0, 0, 0, 0.6f * a});
            glyph(cr, bd.cx(), bd.cy(), "󰐊", 12, Color{1, 1, 1, a});
        }
        if (current) {
            fill(cr, {cx - 5, cy - r * 0.78f, 10, 10}, 5, p.accent.fade(a));
        }
    };

    for (size_t pos = 0; pos < visible_.size(); ++pos) {
        int row = static_cast<int>(pos) / cols_, col = static_cast<int>(pos) % cols_;
        float cx = gx0 + col * (hw + gap) + (row % 2 ? (hw + gap) / 2 : 0);
        float cy = grid_y0_ + row * row_step_ - scroll_.value;
        if (cy + R < top_clip || cy - R > bottom_clip) continue;
        // Staggered entrance, rippling out from the top-left.
        float delay = std::min(0.55f, 0.018f * (row * 2 + col));
        float t = clamp01(static_cast<float>((since - delay) / 0.42));
        if (t < 1) still_entering = true;
        float pop = 0.55f + 0.45f * ease_out_back(t);
        uint64_t id = ID("wp.tile", static_cast<int64_t>(pos));
        float hv = ui_.hover(id);
        float press = ui_.press(id);
        float s = pop * (1.0f + 0.10f * hv + (static_cast<int>(pos) == sel_ ? 0.03f : 0)) * (1.0f - 0.05f * press);
        if (hv > 0.02f && hot_pos < 0) {
            hot_pos = static_cast<int>(pos);
            hot_cx = cx;
            hot_cy = cy;
            hot_s = s;
            // drawn last so it rises above its neighbours
        } else {
            draw_tile(static_cast<int>(pos), cx, cy, s, clamp01(t * 1.5f), false);
        }
        int index = visible_[pos];
        int p_i = static_cast<int>(pos);
        ui_.add(id, {cx - hw / 2, cy - R * 0.75f, hw, R * 1.5f})
            .click([this, index, p_i] {
                sel_ = p_i;
                apply(index, false);
            })
            .right([this, index, p_i] {
                sel_ = p_i;
                apply(index, true);
            });
    }
    if (hot_pos >= 0) draw_tile(hot_pos, hot_cx, hot_cy, hot_s, 1.0f, true);
    entering_ = still_entering;

    ui_.pop_clip();
    cairo_restore(cr);

    // Name of the tile under the pointer (or selected).
    int label_pos = hot_pos >= 0 ? hot_pos : sel_;
    if (label_pos >= 0 && label_pos < static_cast<int>(visible_.size())) {
        int row = label_pos / cols_, col = label_pos % cols_;
        float cx = gx0 + col * (hw + gap) + (row % 2 ? (hw + gap) / 2 : 0);
        float cy = grid_y0_ + row * row_step_ - scroll_.value;
        const Wallpaper& wp = walls_[visible_[label_pos]];
        Text lt{ui(12.5f, "SemiBold"), Color{1, 1, 1, 1}};
        std::string label = wp.name;
        if (utf8_length(label) > 42) label = label.substr(0, 40) + "…";
        Size ls = measure(label, lt);
        Rect pill{cx - ls.w / 2 - 12, cy + R * 1.02f, ls.w + 24, 26};
        if (pill.bottom() < bottom_clip && pill.y > top_clip) {
            fill(cr, pill, 13, Color{0, 0, 0, 0.72f});
            text(cr, pill.x + 12, pill.y + 5, label, lt);
        }
    }

    // ── footer ───────────────────────────────────────────────────────
    int imgs = 0, vids = 0;
    for (auto& wp : walls_) (wp.video ? vids : imgs)++;
    std::string info = std::to_string(visible_.size()) + " de " + std::to_string(walls_.size()) + "   ·   " +
                       std::to_string(imgs) + " imágenes, " + std::to_string(vids) + " vídeos";
    if (thumbs_left_ > 0) info += "   ·   preparando " + std::to_string(thumbs_left_) + " miniaturas…";
    text(cr, w / 2, h - 40, info, {ui(12), Color{1, 1, 1, 0.7f}, Align::Center});
    text(cr, w / 2, h - 22, "Enter aplica  ·  clic derecho previsualiza  ·  flechas navegan  ·  Tab cambia tipo  ·  Ctrl+R aleatorio  ·  Esc cierra",
         {ui(11), Color{1, 1, 1, 0.42f}, Align::Center});

    if (walls_.empty()) {
        text(cr, w / 2, h / 2 - 10, scanning_ ? "Buscando fondos…" : "No hay fondos en las carpetas configuradas",
             {ui(15, "SemiBold"), Color{1, 1, 1, 0.8f}, Align::Center});
        std::string dirs;
        for (auto& d : config().wallpaper_dirs) dirs += (dirs.empty() ? "" : ", ") + d;
        text(cr, w / 2, h / 2 + 16, dirs, {ui(12), Color{1, 1, 1, 0.5f}, Align::Center});
    }

    // Last row fully above the footer at the end of the scroll.
    float maxs = std::max(0.0f, grid_y0_ + (rows - 1) * row_step_ + R + 36 - bottom_clip);
    max_scroll_ = maxs;
    if (scroll_.target > maxs) scroll_.set(maxs);
    if (maxs > 0) {
        float track_h = bottom_clip - top_clip - 40;
        float th = std::max(40.0f, track_h * (track_h / (content_h_ + 1)));
        float ty = top_clip + 20 + (track_h - th) * clamp01(scroll_.value / maxs);
        fill(cr, {w - 12, ty, 4, th}, 2, Color{1, 1, 1, 0.3f});
    }
}

}  // namespace island
