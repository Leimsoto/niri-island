#include "suite/wallpicker.hpp"

#include <dirent.h>
#include <malloc.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <unordered_map>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "services/engine.hpp"
#include "services/steam.hpp"
#include "services/workshop.hpp"
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

// Builds (or reuses) a thumbnail and measures its colour. Worker thread.
// Has an alpha channel and really uses it (most PNG wallpapers carry one, opaque).
bool truly_transparent(GdkPixbuf* pb) {
    if (!gdk_pixbuf_get_has_alpha(pb) || gdk_pixbuf_get_n_channels(pb) != 4) return false;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), stride = gdk_pixbuf_get_rowstride(pb);
    const guchar* px = gdk_pixbuf_read_pixels(pb);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (px[y * stride + x * 4 + 3] < 250) return true;
    return false;
}

// The same picture without its (unused) alpha channel, for JPEG.
GdkPixbuf* opaque_copy(GdkPixbuf* pb) {
    if (!gdk_pixbuf_get_has_alpha(pb)) return GDK_PIXBUF(g_object_ref(pb));
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    GdkPixbuf* out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    const guchar* src = gdk_pixbuf_read_pixels(pb);
    guchar* dst = gdk_pixbuf_get_pixels(out);
    int ss = gdk_pixbuf_get_rowstride(pb), ds = gdk_pixbuf_get_rowstride(out);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) dst[y * ds + x * 3 + k] = src[y * ss + x * 4 + k];
    return out;
}

// Small and good: JPEG at quality 86 unless the picture is really see-through.
bool save_thumb(GdkPixbuf* pb, const std::string& jpg, const std::string& png, std::string& out) {
    if (truly_transparent(pb)) {
        out = png;
        return gdk_pixbuf_save(pb, png.c_str(), "png", nullptr, nullptr);
    }
    GdkPixbuf* rgb = opaque_copy(pb);
    out = jpg;
    gboolean ok = gdk_pixbuf_save(rgb, jpg.c_str(), "jpeg", nullptr, "quality", "86", nullptr);
    g_object_unref(rgb);
    return ok;
}

bool make_thumb(Wallpaper& w, const std::string& dir) {
    char name[40];
    // (Wallpaper Engine items: rendered by the engine, a new name for the old GIF-made ones)
    std::string key = w.path + std::to_string(w.mtime) + (w.engine ? "#engine" : "");
    snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash_str(key)));
    std::string jpg = dir + "/" + name + ".jpg", png = dir + "/" + name + ".png";
    std::string out = file_exists(jpg) ? jpg : file_exists(png) ? png : "";
    if (out == png) {
        // an older PNG thumbnail of an opaque picture: once, to JPEG (a quarter the size)
        if (GdkPixbuf* pb = gdk_pixbuf_new_from_file(png.c_str(), nullptr)) {
            std::string now;
            if (!truly_transparent(pb) && save_thumb(pb, jpg, png, now)) {
                unlink(png.c_str());
                out = jpg;
            }
            g_object_unref(pb);
        }
    }
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
            // A Wallpaper Engine item: the engine renders a sharp still (its
            // preview is a GIF of 192 px or so, which stays the fallback).
            std::string src = w.engine ? w.preview : w.path, still;
            if (w.engine) {
                still = dir + "/" + name + ".still.png";
                if (engine::capture(w.path, still)) src = still;
            }
            GError* err = nullptr;
            GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(src.c_str(), 520, 520, TRUE, &err);
            if (!pb) {
                if (err) g_error_free(err);
                return false;
            }
            bool ok = save_thumb(pb, jpg, png, out);
            g_object_unref(pb);
            if (!still.empty()) unlink(still.c_str());
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

}  // namespace

// Thumbnails of pictures no longer there (deleted, edited) go.
void WallPicker::prune_thumbs(const std::string& dir) {
    auto keep = std::make_shared<std::unordered_set<std::string>>();
    for (auto& w : walls_)
        if (!w.thumb.empty()) keep->insert(w.thumb);
    for (auto& w : local_walls_)
        if (!w.thumb.empty()) keep->insert(w.thumb);
    if (keep->empty()) return;  // nothing scanned: better not to guess
    Loop::get().async([dir, keep] {
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        while (dirent* e = readdir(d)) {
            std::string f = e->d_name;
            if (f == "." || f == "..") continue;
            if (!keep->count(dir + "/" + f)) unlink((dir + "/" + f).c_str());
        }
        closedir(d);
    });
}

std::string WallPicker::current_wallpaper() const {
    std::string text;
    if (!read_file(expand_home(config().wallpaper_file), text) && !read_file(state_dir() + "/wallpaper.json", text)) return "";
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j.value("path", "") : "";
}

WallPicker::WallPicker(Wayland& wl) : Overlay(wl) {}
WallPicker::~WallPicker() { free_caches(); }

void WallPicker::show() {
    if (exists()) return;
    // slices (carousel) | hex (skwd-wall's honeycomb; "grid" before) | cards (wallhaven)
    const std::string& st = config().wallpaper_picker_style;
    style_ = st == "cards" ? 2 : (st == "hex" || st == "grid") ? 1 : 0;
    open();
}

void WallPicker::closed() {
    if (trying_) try_end(false);  // closed while trying: the previous one comes back
    if (steam_proc_) steam_proc_->stop();
    free_anim();
    // keep the slices and lights (the next opening is instant); drop the big copy
    if (big_fit_) {
        cairo_surface_destroy(big_fit_);
        big_fit_ = nullptr;
    }
    big_path_.clear();
    // Not opened again for a while: let the pictures and lights go (they
    // are made again, by workers, the next time).
    Loop::get().cancel(release_timer_);
    release_timer_ = Loop::get().after(120.0, [this] {
        release_timer_ = 0;
        if (!exists()) {
            free_caches();
            malloc_trim(0);
        }
    });
}

void WallPicker::opened() {
    open_t0_ = now_mono();
    inserting_ = inserted_ = moved_ = leaving_ = left_ = false;
    if (remote_ || steam_) {  // always open on the local folders
        remote_ = steam_ = ws_browse_ = false;
        walls_ = std::move(local_walls_);
        local_walls_.clear();
    }
    panel_ = trying_ = try_busy_ = false;
    panel_t_.reset(0);
    try_t_.reset(0);
    panel_index_ = -1;
    downloading_ = false;
    backdrop_ready_ = false;
    applied_path_.clear();
    filter_t0_ = 0;
    kind_ = 0;
    hue_ = -1;
    current_ = current_wallpaper();
    search_.clear();
    if (walls_.empty()) scan();
    else {
        refilter();
        scan();  // refresh in the background: new files, new thumbs
    }
    // Start the selection on the wallpaper in use.
    for (size_t i = 0; i < visible_.size(); ++i)
        if (walls_[visible_[i]].path == current_) sel_ = static_cast<int>(i);
    if (workshop::is_item(current_)) set_steam(true);  // open where the current one is
    pos_.reset(static_cast<float>(std::max(0, sel_)));
    colpos_.reset(static_cast<float>(std::max(0, sel_) / kTileRows));
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
            if (config().wallpaper_engine && config().wallpaper_engine_clean) workshop::clean_workshop();
            if (config().wallpaper_engine)  // downloaded Workshop items
                for (auto& it : workshop::items()) {
                    if (it.preview.empty()) continue;
                    struct stat st {};
                    stat((it.dir + "/project.json").c_str(), &st);
                    Wallpaper w;
                    w.path = it.dir;
                    w.name = it.title;
                    w.preview = it.preview;
                    w.engine = true;
                    w.ws_type = it.type;
                    w.ws_id = it.dir.substr(it.dir.rfind('/') + 1);
                    w.mtime = static_cast<int64_t>(st.st_mtime);
                    found->push_back(std::move(w));
                }
            std::sort(found->begin(), found->end(), [](const Wallpaper& a, const Wallpaper& b) {
                if (a.engine != b.engine) return !a.engine;
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
            has_engine_ = std::any_of(found->begin(), found->end(), [](const Wallpaper& w) { return w.engine; });
            if (remote_ || steam_) {  // browsing wallhaven or Steam: the local list waits
                local_walls_ = std::move(*found);
                if (steam_ && !ws_browse_) ws_downloads();  // (its downloads come from it)
                scanning_ = false;
                request();
                return;
            }
            walls_ = std::move(*found);
            refilter();
            for (size_t i = 0; i < visible_.size(); ++i)
                if (walls_[visible_[i]].path == current_ && (sel_ < 0 || !moved_)) sel_ = static_cast<int>(i);
            if (!moved_) {
                pos_.reset(static_cast<float>(std::max(0, sel_)));
                colpos_.reset(static_cast<float>(std::max(0, sel_) / kTileRows));
            }
            request();

            // Thumbnails still missing, in small batches so the grid fills in.
            auto todo = std::make_shared<std::vector<Wallpaper>>();
            for (auto& w : walls_)  // (older PNG thumbnails go through it once, to JPEG)
                if (w.thumb.empty() || w.hue < 0 || ends_with(w.thumb, ".png")) todo->push_back(w);
            thumbs_left_ = static_cast<int>(todo->size());
            if (todo->empty()) {
                scanning_ = false;
                prune_thumbs(thumbs);
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
                    [this, batch, from, todo, index_path, run_batch, thumbs] {
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
                        prune_thumbs(thumbs);
                        refilter();
                        request();
                    });
            };
            (*run_batch)(0);
        });
}

void WallPicker::refilter() {
    std::string q = lower(search_);
    // (visible_ may still index a list just swapped out: check before reading)
    std::string keep = sel_ >= 0 && sel_ < static_cast<int>(visible_.size()) &&
                               visible_[sel_] < static_cast<int>(walls_.size())
                           ? walls_[visible_[sel_]].path
                           : "";
    visible_.clear();
    for (size_t i = 0; i < walls_.size(); ++i) {
        const auto& w = walls_[i];
        if (w.remote || (steam_ && ws_browse_)) {  // wallhaven and the Workshop filter on their side
            visible_.push_back(static_cast<int>(i));
            continue;
        }
        if (steam_) {  // the downloads: by name, as typed
            if (!ws_query_.empty() && lower(w.name).find(lower(ws_query_)) == std::string::npos) continue;
            visible_.push_back(static_cast<int>(i));
            continue;
        }
        if (w.engine) continue;  // Wallpaper Engine lives in the Steam source
        if (kind_ == 1 && w.moving()) continue;
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
    // (Wallpaper Engine items always go through the backdrop: it runs the engine)
    if ((w.engine || cmd.empty() || cmd == "internal" || cmd.find("skwd") != std::string::npos) && on_apply) {
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

void WallPicker::canvas_scroll(float dy) {
    if (grid_mode()) {
        float max = std::max(0.0f, grid_content_h() - grid_view_h_);
        grid_scroll_.set(std::clamp(grid_scroll_.target + dy * 90, 0.0f, max));
        if (grid_scroll_.target > max - grid_row_h_ * 2) {
            if (remote_) wh_fetch(true);
            else ws_fetch(true);
        }
        request();
        return;
    }
    // the grids scroll a column at a time, the carousel one slice
    slices_move((dy > 0 ? 1 : -1) * (style_ == 0 ? 1 : kTileRows));
}

bool WallPicker::animating() const {
    return inserting_ || leaving_ || wh_loading_ || ws_loading_ || grid_mode() || trying_ || !pos_.settled() ||
           !colpos_.settled() || !panel_t_.settled() || !try_t_.settled() ||
           now_mono() - open_t0_ < 1.3 || (filter_t0_ > 0 && now_mono() - filter_t0_ < 0.4);
}

void WallPicker::key(xkb_keysym_t sym, const std::string& utf8, uint32_t) { key_slices(sym, utf8); }

void WallPicker::draw(cairo_t* cr, float w, float h, float dt) { draw_slices(cr, w, h, dt); }

}  // namespace island
