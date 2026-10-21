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

// Flat-topped hexagon (skwd-wall's honeycomb).
void hex_flat_path(cairo_t* cr, float cx, float cy, float r) {
    cairo_new_path(cr);
    for (int i = 0; i < 6; ++i) {
        double a = M_PI / 180.0 * (60 * i);
        double x = cx + r * std::cos(a), y = cy + r * std::sin(a);
        if (i == 0) cairo_move_to(cr, x, y);
        else cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}


}  // namespace

std::string WallPicker::current_wallpaper() const {
    std::string text;
    if (!read_file(expand_home(config().wallpaper_file), text) && !read_file(state_dir() + "/wallpaper.json", text)) return "";
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j.value("path", "") : "";
}

WallPicker::~WallPicker() {
    free_caches();
    free_tiles();
}

void WallPicker::free_tiles() {
    for (auto& [k, s] : tiles_) cairo_surface_destroy(s);
    tiles_.clear();
    if (grid_veil_) cairo_surface_destroy(grid_veil_);
    grid_veil_ = nullptr;
}

cairo_surface_t* WallPicker::tile_image(const std::string& thumb, float tw, float th, bool hex) {
    int sc = std::max(1, scale());
    std::string key = std::to_string(static_cast<int>(tw)) + "x" + std::to_string(static_cast<int>(th)) + (hex ? "h" : "c") +
                      std::to_string(sc);
    if (key != tiles_key_) {  // another size or style: start over
        free_tiles();
        tiles_key_ = key;
    }
    if (auto it = tiles_.find(thumb); it != tiles_.end()) return it->second;
    cairo_surface_t* src = ImageCache::get().request(thumb, 420);
    if (!src) return nullptr;  // not decoded yet: next frame
    if (tiles_.size() > 160) free_tiles();  // bounded: they are cheap to remake
    int pw = static_cast<int>(std::ceil(tw)) + 2, ph = static_cast<int>(std::ceil(th)) + 2;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw * sc, ph * sc);
    cairo_surface_set_device_scale(s, sc, sc);
    cairo_t* c = cairo_create(s);
    float cx = pw / 2.0f, cy = ph / 2.0f;
    if (hex) hex_flat_path(c, cx, cy, tw / 2);
    else rrect(c, {cx - tw / 2, cy - th / 2, tw, th}, 12);
    cairo_clip(c);
    image(c, src, {cx - tw / 2, cy - th / 2, tw, th}, 0);
    cairo_destroy(c);
    tiles_[thumb] = s;
    return s;
}

void WallPicker::show() {
    if (exists()) return;
    // slices (carousel) | hex (skwd-wall's honeycomb; "grid" before) | cards (wallhaven)
    const std::string& st = config().wallpaper_picker_style;
    style_ = st == "cards" ? 2 : (st == "hex" || st == "grid") ? 1 : 0;
    slices_ = style_ == 0;
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
    // Made once per size: the full-screen radial gradient cost ~24 ms a frame.
    int sc = std::max(1, scale());
    std::string vkey = std::to_string(static_cast<int>(w)) + "x" + std::to_string(static_cast<int>(h)) + "/" +
                       std::to_string(style_) + "/" + std::to_string(sc);
    if (!grid_veil_ || grid_veil_key_ != vkey) {
        if (grid_veil_) cairo_surface_destroy(grid_veil_);
        grid_veil_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(w) * sc, static_cast<int>(h) * sc);
        cairo_surface_set_device_scale(grid_veil_, sc, sc);
        cairo_t* vc = cairo_create(grid_veil_);
        cairo_set_source_rgba(vc, 0.02, 0.02, 0.03, style_ == 2 ? 0.86 : 0.5);
        cairo_paint(vc);
        cairo_pattern_t* vg = cairo_pattern_create_radial(w / 2, h / 2, h * 0.25, w / 2, h / 2, w * 0.75);
        cairo_pattern_add_color_stop_rgba(vg, 0, 0, 0, 0, 0);
        cairo_pattern_add_color_stop_rgba(vg, 1, 0, 0, 0, 0.45);
        cairo_set_source(vc, vg);
        cairo_paint(vc);
        cairo_pattern_destroy(vg);
        cairo_destroy(vc);
        grid_veil_key_ = vkey;
    }
    cairo_set_source_surface(cr, grid_veil_, 0, 0);
    cairo_paint(cr);
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

    // ── grid: the honeycomb (skwd-wall) or wallhaven's cards ─────────
    bool cards = style_ == 2;
    float top_clip = bar.bottom() + 14, bottom_clip = h - 54;
    top_clip_ = top_clip;
    bottom_clip_ = bottom_clip;
    int n = static_cast<int>(visible_.size());
    float R = 0, tw = 0, th = 0, colstep = 0, x0 = 0;
    if (cards) {
        float gw = std::min(w - 80, 1560.0f), gap = 16;
        cols_ = gw > 1300 ? 5 : gw > 1000 ? 4 : 3;
        tw = std::floor((gw - gap * (cols_ - 1)) / cols_);
        th = std::round(tw * 9 / 16);
        colstep = tw + gap;
        row_step_ = th + gap;
        x0 = std::round((w - (cols_ * colstep - gap)) / 2 + tw / 2);
        grid_y0_ = bar.bottom() + 30 + th / 2;
        radius_ = th / 2;
    } else {
        // flat-topped hexagons nearly edge to edge, odd columns half a row down
        R = std::clamp(h / 7.4f, 90.0f, 170.0f);
        float gap = 5;
        tw = 2 * R;
        th = std::sqrt(3.0f) * R;
        colstep = 1.5f * R + gap * 0.87f;
        row_step_ = th + gap;
        cols_ = std::max(1, static_cast<int>((w - 40 - tw) / colstep) + 1);
        x0 = (w - ((cols_ - 1) * colstep + tw)) / 2 + R;
        grid_y0_ = bar.bottom() + 26 + th / 2;
        radius_ = th / 2 + row_step_ / 4;
    }
    int rows = (n + cols_ - 1) / cols_;
    content_h_ = rows * row_step_ + (cards ? 0 : row_step_ / 2);
    auto centre = [&](int pos, float& cx, float& cy) {
        int row = pos / cols_, col = pos % cols_;
        cx = x0 + col * colstep;
        cy = grid_y0_ + row * row_step_ + (!cards && col % 2 ? row_step_ / 2 : 0) - scroll_.value;
    };
    auto shape = [&](float cx, float cy, float s) {
        if (cards) rrect(cr, {cx - tw * s / 2, cy - th * s / 2, tw * s, th * s}, 12 * s);
        else hex_flat_path(cr, cx, cy, R * s);
    };

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
        float bw = tw * s, bh = th * s;
        bool current = wp.path == current_;
        bool sel = pos == sel_;
        if (lifted) {  // a soft shadow under the lifted tile
            shape(cx, cy + 7, s * 1.02f);
            cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * a);
            cairo_fill(cr);
        }
        cairo_surface_t* img = wp.thumb.empty() ? nullptr : tile_image(wp.thumb, tw, th, !cards);
        if (img) {
            // the ready-made tile, only scaled when it grows or pops in
            int iw = cairo_image_surface_get_width(img) / std::max(1, scale());
            int ih = cairo_image_surface_get_height(img) / std::max(1, scale());
            cairo_save(cr);
            bool scaled = std::abs(s - 1) > 0.002f;
            if (scaled) {
                cairo_translate(cr, cx, cy);
                cairo_scale(cr, s, s);
                cairo_set_source_surface(cr, img, -iw / 2.0, -ih / 2.0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            } else {
                // whole pixels: a straight copy, no resampling (the slow part)
                cairo_set_source_surface(cr, img, std::round(cx - iw / 2.0), std::round(cy - ih / 2.0));
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
            }
            if (a >= 0.999f) cairo_paint(cr);
            else cairo_paint_with_alpha(cr, a);
            cairo_restore(cr);
        } else {
            shape(cx, cy, s);
            set(cr, p.surface_hi.with_alpha(a));
            cairo_fill(cr);
            glyph(cr, cx, cy, wp.video ? "󰕧" : "󰋩", bh * 0.18f, p.text_faint.fade(a));
        }
        cairo_save(cr);
        shape(cx, cy, s);
        cairo_clip(cr);
        if (cards && (lifted || sel)) {
            // wallhaven's hover: a dark foot with the name and the kind
            float fh = 44;
            cairo_pattern_t* g = cairo_pattern_create_linear(0, cy + bh / 2 - fh * 1.6f, 0, cy + bh / 2);
            cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0);
            cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0.78 * a);
            cairo_set_source(cr, g);
            cairo_rectangle(cr, cx - bw / 2, cy + bh / 2 - fh * 1.6f, bw, fh * 1.6f);
            cairo_fill(cr);
            cairo_pattern_destroy(g);
            std::string kind = wp.video ? "Vídeo" : "Imagen";
            if (!wp.resolution.empty()) kind += "  ·  " + wp.resolution;
            text(cr, cx - bw / 2 + 12, cy + bh / 2 - 40, wp.name, {ui(12.5f, "SemiBold"), Color{1, 1, 1, a}, Align::Left, bw - 24});
            text(cr, cx - bw / 2 + 12, cy + bh / 2 - 21, kind, {ui(11), Color{1, 1, 1, 0.7f * a}, Align::Left, bw - 24});
        }
        cairo_restore(cr);
        // the rim
        shape(cx, cy, s);
        if (current || sel || lifted) {
            set(cr, (current ? p.accent : Color{1, 1, 1, 1}).with_alpha((sel || lifted ? 0.95f : 0.85f) * a));
            cairo_set_line_width(cr, current ? 3.5 : 2.5);
        } else if (cards) {
            set(cr, Color{1, 1, 1, 0.08f * a});
            cairo_set_line_width(cr, 1);
        } else {
            set(cr, Color{1, 1, 1, 0.55f * a});  // skwd-wall's light seams
            cairo_set_line_width(cr, 1.5);
        }
        cairo_stroke(cr);
        if (wp.video) {
            Rect bd = cards ? Rect{cx + bw / 2 - 40, cy - bh / 2 + 10, 30, 20} : Rect{cx - 14, cy + bh * 0.3f, 28, 20};
            fill(cr, bd, 10, Color{0, 0, 0, 0.6f * a});
            glyph(cr, bd.cx(), bd.cy(), "󰐊", 12, Color{1, 1, 1, a});
        }
        if (current) {
            if (cards) {
                Rect badge{cx - bw / 2 + 10, cy - bh / 2 + 10, 64, 22};
                fill(cr, badge, 11, p.accent.fade(a));
                text(cr, badge.cx(), badge.y + 3, "En uso", {ui(11, "Bold"), p.on_accent.fade(a), Align::Center});
            } else {
                fill(cr, {cx - 5, cy - bh * 0.4f, 10, 10}, 5, p.accent.fade(a));
            }
        }
    };

    for (int pos = 0; pos < n; ++pos) {
        float cx, cy;
        centre(pos, cx, cy);
        if (cy + th / 2 < top_clip || cy - th / 2 > bottom_clip) continue;
        int row = pos / cols_, col = pos % cols_;
        // Staggered entrance, rippling out from the top-left.
        float delay = std::min(0.55f, 0.018f * (row * 2 + col));
        float t = clamp01(static_cast<float>((since - delay) / 0.42));
        if (t < 1) still_entering = true;
        // in: rising and fading (a shift of a ready tile is nearly free; a
        // scale of ~35 of them every frame was not)
        float e = 1 - (1 - t) * (1 - t) * (1 - t);
        cy += std::round((1 - e) * 28);
        uint64_t id = ID("wp.tile", pos);
        float hv = ui_.hover(id);
        float press = ui_.press(id);
        float grow = cards ? 0.04f : 0.08f;
        float s = (1.0f + grow * hv + (pos == sel_ ? 0.02f : 0)) * (1.0f - 0.05f * press);
        if (hv > 0.02f && hot_pos < 0) {
            hot_pos = pos;  // drawn last so it rises above its neighbours
            hot_cx = cx;
            hot_cy = cy;
            hot_s = s;
        } else {
            // opaque: a see-through tile costs ~1 ms each to blend
            if (t > 0) draw_tile(pos, cx, cy, s, 1.0f, false);
        }
        int index = visible_[pos];
        Rect hit = cards ? Rect{cx - tw / 2, cy - th / 2, tw, th} : Rect{cx - R * 0.75f, cy - th / 2, R * 1.5f, th};
        ui_.add(id, hit)
            .click([this, index, pos] {
                sel_ = pos;
                apply(index, false);
            })
            .right([this, index, pos] {
                sel_ = pos;
                apply(index, true);
            });
    }
    if (hot_pos >= 0) draw_tile(hot_pos, hot_cx, hot_cy, hot_s, 1.0f, true);
    entering_ = still_entering;

    ui_.pop_clip();
    cairo_restore(cr);

    // Name of the tile under the pointer (or selected); the cards carry their own.
    int label_pos = hot_pos >= 0 ? hot_pos : sel_;
    if (!cards && label_pos >= 0 && label_pos < n) {
        float cx, cy;
        centre(label_pos, cx, cy);
        const Wallpaper& wp = walls_[visible_[label_pos]];
        Text lt{ui(12.5f, "SemiBold"), Color{1, 1, 1, 1}};
        std::string label = wp.name;
        if (utf8_length(label) > 42) label = label.substr(0, 40) + "…";
        Size ls = measure(label, lt);
        Rect pill{cx - ls.w / 2 - 12, cy + th / 2 + 6, ls.w + 24, 26};
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
    float maxs = std::max(0.0f, grid_y0_ + (rows - 1) * row_step_ + th / 2 + (cards ? 0 : row_step_ / 2) + 36 - bottom_clip);
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
