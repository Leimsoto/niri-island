// The picker's Steam source and its side panel.
//
// Steam: the Wallpaper Engine items Steam has downloaded, in a grid like
// wallhaven's; one button over, the Workshop itself (search, order, get).
// The Workshop goes through niri-island-steam, alive while it is shown.
//
// The panel: one click on a card (Steam or wallhaven) slides it in on the
// right, with a large (animated, when the preview is) picture and the few
// things to do with it; a second click on the card does the main one.
//
// Trying: the picker steps aside to show it on the desktop for real; ✓ keeps
// it, ✕ puts the previous one back.

#include <atomic>
#include <cmath>
#include <dirent.h>
#include <filesystem>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <sys/stat.h>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "services/engine.hpp"
#include "services/images.hpp"
#include "services/steam.hpp"
#include "services/workshop.hpp"
#include "suite/wallpicker.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace island {

using namespace paint;
using json = nlohmann::json;

namespace {

std::string base_name(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Covering `r` with `img`, cropped to its middle.
void cover_rect(cairo_t* cr, cairo_surface_t* img, const Rect& r, float alpha) {
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    double sx, sy;
    cairo_surface_get_device_scale(img, &sx, &sy);
    float w = iw / static_cast<float>(sx), h = ih / static_cast<float>(sy);
    if (w <= 0 || h <= 0) return;
    float s = std::max(r.w / w, r.h / h);
    cairo_save(cr);
    cairo_rectangle(cr, r.x, r.y, r.w, r.h);
    cairo_clip(cr);
    cairo_translate(cr, r.cx() - w * s / 2, r.cy() - h * s / 2);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

// A GdkPixbuf as a cairo surface covering w×h (premultiplied, its middle).
cairo_surface_t* pixbuf_cover(GdkPixbuf* pb, int w, int h) {
    int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
    float s = std::max(w / static_cast<float>(pw), h / static_cast<float>(ph));
    int sw = std::max(1, static_cast<int>(std::lround(pw * s))), sh = std::max(1, static_cast<int>(std::lround(ph * s)));
    GdkPixbuf* scaled = gdk_pixbuf_scale_simple(pb, sw, sh, GDK_INTERP_BILINEAR);
    if (!scaled) return nullptr;
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    unsigned char* dst = cairo_image_surface_get_data(out);
    int ds = cairo_image_surface_get_stride(out);
    const unsigned char* src = gdk_pixbuf_get_pixels(scaled);
    int ss = gdk_pixbuf_get_rowstride(scaled), nc = gdk_pixbuf_get_n_channels(scaled);
    bool alpha = gdk_pixbuf_get_has_alpha(scaled);
    int ox = (sw - w) / 2, oy = (sh - h) / 2;
    for (int y = 0; y < h; ++y) {
        const unsigned char* sr = src + (y + oy) * ss;
        auto* dr = reinterpret_cast<uint32_t*>(dst + y * ds);
        for (int x = 0; x < w; ++x) {
            const unsigned char* px = sr + (x + ox) * nc;
            uint32_t a = alpha ? px[3] : 255;
            uint32_t r = px[0] * a / 255, g = px[1] * a / 255, b = px[2] * a / 255;
            dr[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(out);
    g_object_unref(scaled);
    return out;
}

// The frames of an animated preview (a GIF), up to ~8 s or 64 frames, at w×h.
// Nothing for a still picture, or one narrower than `min_w`.
void load_frames(const std::string& path, int w, int h, int min_w, std::vector<cairo_surface_t*>& frames,
                 std::vector<int>& ms) {
    GdkPixbufAnimation* anim = gdk_pixbuf_animation_new_from_file(path.c_str(), nullptr);
    if (!anim) return;
    if (gdk_pixbuf_animation_is_static_image(anim) || gdk_pixbuf_animation_get_width(anim) < min_w) {
        g_object_unref(anim);
        return;
    }
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GTimeVal t{0, 0};
    GdkPixbufAnimationIter* it = gdk_pixbuf_animation_get_iter(anim, &t);
    int total = 0;
    for (int i = 0; i < 64 && total < 8000; ++i) {
        int delay = gdk_pixbuf_animation_iter_get_delay_time(it);
        if (cairo_surface_t* s = pixbuf_cover(gdk_pixbuf_animation_iter_get_pixbuf(it), w, h)) {
            frames.push_back(s);
            ms.push_back(std::max(20, delay));
        }
        if (delay < 0) break;
        total += std::max(20, delay);
        g_time_val_add(&t, std::max(20, delay) * 1000L);
        gdk_pixbuf_animation_iter_advance(it, &t);
    }
    G_GNUC_END_IGNORE_DEPRECATIONS
    g_object_unref(it);
    g_object_unref(anim);
}

const char* type_glyph(const std::string& t) {
    if (t == "scene") return "󰆧";
    if (t == "video") return "󰕧";
    if (t == "web") return "󰖟";
    return "󰋩";
}

std::string human_size(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) snprintf(buf, sizeof(buf), "%.1f GB", b / double(1ull << 30));
    else snprintf(buf, sizeof(buf), "%.0f MB", std::max(1.0, b / double(1ull << 20)));
    return buf;
}

std::string human_count(uint64_t n) {
    char buf[32];
    if (n >= 1000000) snprintf(buf, sizeof(buf), "%.1fM", n / 1e6);
    else if (n >= 1000) snprintf(buf, sizeof(buf), "%.0fk", n / 1e3);
    else snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(n));
    return buf;
}

// A download's progress: a ring round the middle of `r`.
void ring(cairo_t* cr, float cx, float cy, float rad, float progress, Color c, float alpha) {
    cairo_set_line_width(cr, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.45 * alpha);
    cairo_arc(cr, cx, cy, rad + 6, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.18 * alpha);
    cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
    cairo_stroke(cr);
    float p = progress > 0 ? progress : 0.08f;
    float start = progress > 0 ? -M_PI / 2 : static_cast<float>(std::fmod(now_mono() * 5, 2 * M_PI));
    cairo_set_source_rgba(cr, c.r, c.g, c.b, alpha);
    cairo_arc(cr, cx, cy, rad, start, start + 2 * M_PI * p);
    cairo_stroke(cr);
}

}  // namespace

// ── the Steam source ──────────────────────────────────────────────────────

void WallPicker::show_engine(bool on) { set_steam(on); }

void WallPicker::set_steam(bool on) {
    if (on == steam_ || inserting_ || leaving_) return;
    if (on && remote_) set_remote(false);
    close_panel();
    steam_ = on;
    ++ws_gen_;
    ws_loading_ = false;
    if (on) {
        ws_browse_ = false;
        ws_details_asked_ = false;
        ws_query_.clear();
        local_walls_ = std::move(walls_);
        walls_.clear();
        visible_.clear();
        ws_downloads();
    } else {
        if (steam_proc_) steam_proc_->stop();
        walls_ = std::move(local_walls_);
        local_walls_.clear();
        visible_.clear();
        refilter();
        sel_ = 0;
        for (size_t i = 0; i < visible_.size(); ++i)
            if (walls_[visible_[i]].path == current_) sel_ = static_cast<int>(i);
    }
    pos_.reset(static_cast<float>(std::max(0, sel_)));
    colpos_.reset(static_cast<float>(std::max(0, sel_) / kTileRows));
    grid_scroll_.reset(0);
    grid_t0_ = filter_t0_ = now_mono();
    request();
}

// What Steam has downloaded (the Wallpaper Engine items of the folders' scan).
void WallPicker::ws_downloads() {
    std::string keep = sel_ >= 0 && sel_ < static_cast<int>(visible_.size()) &&
                               visible_[sel_] < static_cast<int>(walls_.size())
                           ? walls_[visible_[sel_]].path
                           : current_;
    walls_.clear();
    visible_.clear();
    for (const auto& w : local_walls_)
        if (w.engine) {
            walls_.push_back(w);
            walls_.back().installed = walls_.back().subscribed = true;
        }
    refilter();
    for (size_t i = 0; i < visible_.size(); ++i)
        if (walls_[visible_[i]].path == keep) sel_ = static_cast<int>(i);
}

void WallPicker::ws_set_browse(bool on) {
    if (on == ws_browse_) return;
    close_panel();
    ws_browse_ = on;
    ws_query_.clear();
    ++ws_gen_;
    grid_scroll_.reset(0);
    grid_t0_ = filter_t0_ = now_mono();
    if (on) {
        walls_.clear();
        visible_.clear();
        sel_ = 0;
        ws_fetch(false);
    } else {
        ws_downloads();
    }
    pos_.reset(static_cast<float>(std::max(0, sel_)));
    colpos_.reset(static_cast<float>(std::max(0, sel_) / kTileRows));
    request();
}

bool WallPicker::ws_helper() {
    if (steam_proc_ && steam_proc_->running()) return true;
    if (!Steam::installed()) {
        ws_error_ = "missing";
        return false;
    }
    if (!steam_proc_) {
        steam_proc_ = std::make_unique<Steam>();
        steam_proc_->on_event = [this](const json& ev) { ws_event(ev); };
        steam_proc_->on_exit = [this] {
            ws_loading_ = false;
            ws_cancelled_.clear();
            for (auto& w : walls_) w.progress = -1;
            if (steam_ && ws_error_.empty() && exists()) ws_error_ = "closed";
            request();
        };
    }
    ws_error_.clear();
    return steam_proc_->start();
}

void WallPicker::ws_fetch(bool more) {
    if (!steam_ || !ws_browse_) return;
    if (more && (ws_loading_ || !ws_more_ || !ws_error_.empty() || walls_.empty())) return;
    if (!more) {
        ++ws_gen_;
        walls_.clear();
        visible_.clear();
        sel_ = 0;
        ws_page_ = 0;
        ws_more_ = true;
        grid_scroll_.reset(0);
        grid_t0_ = now_mono();
    }
    if (!ws_helper()) {
        request();
        return;
    }
    ws_loading_ = true;
    int page = ws_page_ + 1;
    steam_proc_->search(ws_query_.text, ws_sort_, page, ws_mature_, std::to_string(ws_gen_) + ":" + std::to_string(page));
    request();
}

void WallPicker::ws_event(const json& ev) {
    std::string kind = ev.value("ev", "");
    std::string id = ev.value("id", "");
    auto find = [&](std::vector<Wallpaper>& list) -> Wallpaper* {
        for (auto& w : list)
            if (!id.empty() && w.ws_id == id) return &w;
        return nullptr;
    };
    if (kind == "ready") {
        ws_error_.clear();
    } else if (kind == "results") {
        std::string tag = ev.value("tag", "");
        size_t colon = tag.find(':');
        if (colon == std::string::npos || std::atoi(tag.c_str()) != ws_gen_ || !steam_ || !ws_browse_) return;
        ws_loading_ = false;
        ws_page_ = std::atoi(tag.c_str() + colon + 1);
        const auto& items = ev.contains("items") && ev["items"].is_array() ? ev["items"] : json::array();
        ws_more_ = items.size() >= 30;
        std::vector<std::string> dirs = workshop::workshop_dirs();
        for (const auto& it : items) {
            if (!it.is_object()) continue;
            Wallpaper w;
            w.remote = true;
            w.ws_id = it.value("id", "");
            w.name = it.value("title", "");
            w.thumb = it.value("preview", "");
            w.preview = w.thumb;
            w.ws_type = it.value("type", "");
            w.ws_size = it.value("size", static_cast<uint64_t>(0));
            w.ws_subs = it.value("subs", static_cast<uint64_t>(0));
            w.mature = it.value("mature", false);
            w.subscribed = it.value("subscribed", false);
            w.installed = it.value("installed", false);
            if (w.ws_id.empty()) continue;
            std::string owner = it.value("owner", ""), author = it.value("author", "");
            if (!owner.empty()) ws_owner_of_[w.ws_id] = owner;
            if (!owner.empty() && !author.empty()) ws_names_[owner] = author;
            for (auto& d : dirs)  // on disk already: an ordinary item
                if (w.installed && file_exists(d + "/" + w.ws_id + "/project.json")) {
                    w.remote = false;
                    w.engine = true;
                    w.path = d + "/" + w.ws_id;
                }
            if (w.path.empty()) w.path = "steam:" + w.ws_id;
            walls_.push_back(std::move(w));
            visible_.push_back(static_cast<int>(walls_.size()) - 1);
        }
        ws_thumbs(ws_gen_);
    } else if ((kind == "progress" || kind == "done") && ws_cancelled_.count(id)) {
        return;  // stopped: what was still on its way does not count
    } else if (kind == "details" || kind == "author") {
        // who made them: an account per item, a name per account (maybe later)
        if (kind == "author") {
            std::string owner = ev.value("owner", ""), name = ev.value("name", "");
            if (!owner.empty() && !name.empty()) ws_names_[owner] = name;
        } else if (ev.contains("items") && ev["items"].is_array()) {
            for (const auto& it : ev["items"]) {
                if (!it.is_object()) continue;
                std::string iid = it.value("id", ""), owner = it.value("owner", ""), author = it.value("author", "");
                if (iid.empty() || owner.empty()) continue;
                ws_owner_of_[iid] = owner;
                if (!author.empty()) ws_names_[owner] = author;
            }
        }
    } else if (kind == "progress") {
        float p = ev.value("progress", 0.0f);
        if (Wallpaper* w = find(walls_)) w->progress = p;
    } else if (kind == "done") {
        std::string folder = ev.value("folder", "");
        if (Wallpaper* w = find(walls_)) {
            w->progress = -1;
            w->installed = w->subscribed = true;
            if (!folder.empty()) {
                w->remote = false;
                w->engine = true;
                w->path = folder;
            }
        }
        if (id == ws_apply_after_ && !folder.empty()) {
            ws_apply_after_.clear();
            applied_path_ = current_ = folder;
            if (on_apply) on_apply(folder);
        }
        scan();  // the folders' list takes it in
    } else if (kind == "unsubscribed") {
        if (ws_cancelled_.erase(id) && !id.empty() &&
            id.find_first_not_of("0123456789") == std::string::npos) {
            // a stopped download may have left part of it where the items live
            std::vector<std::string> dirs = workshop::workshop_dirs();
            Loop::get().async(
                [dirs, id] {
                    for (const auto& d : dirs) {
                        std::error_code ec;
                        std::filesystem::remove_all(d + "/" + id, ec);
                    }
                },
                [] {});
        }
        if (!ws_browse_) {
            for (size_t i = 0; i < walls_.size(); ++i)
                if (walls_[i].ws_id == id) {
                    if (panel_index_ == static_cast<int>(i)) close_panel();
                    walls_.erase(walls_.begin() + static_cast<long>(i));
                    break;
                }
            refilter();
        } else if (Wallpaper* w = find(walls_)) {
            w->subscribed = w->installed = false;
            w->remote = true;
            w->engine = false;
            w->path = "steam:" + id;
        }
        scan();
    } else if (kind == "error") {
        std::string msg = ev.value("message", "");
        if (id.empty()) {
            ws_error_ = msg.find("sesión") != std::string::npos ? "signin" : "closed";
            ws_loading_ = false;
            // (the Workshop shows why; elsewhere, a word)
            if (!ws_browse_ && !ws_quiet_ && on_toast) on_toast(ws_error_ == "signin" ? "Inicia sesión en Steam" : "Steam está cerrado");
        } else {
            if (Wallpaper* w = find(walls_)) w->progress = -1;
            ws_cancelled_.erase(id);
            if (id == ws_apply_after_) ws_apply_after_.clear();
            if (on_toast && !msg.empty()) on_toast(msg);
            ws_loading_ = false;
        }
    }
    request();
}

// Subscribe and download (Steam does it); applied when done if asked.
void WallPicker::ws_get(int index, bool apply_after) {
    if (index < 0 || index >= static_cast<int>(walls_.size())) return;
    Wallpaper& w = walls_[index];
    if (w.ws_id.empty() || w.progress >= 0 || !ws_helper()) {
        request();
        return;
    }
    ws_quiet_ = false;
    ws_cancelled_.erase(w.ws_id);
    w.progress = 0;
    if (apply_after) ws_apply_after_ = w.ws_id;
    steam_proc_->subscribe(w.ws_id);
    request();
}

// Unsubscribe (on Steam) and delete the download (here).
void WallPicker::ws_unsubscribe(int index) {
    if (index < 0 || index >= static_cast<int>(walls_.size())) return;
    std::string id = walls_[index].ws_id.empty() ? base_name(walls_[index].path) : walls_[index].ws_id;
    if (id.empty() || !ws_helper()) {
        request();
        return;
    }
    ws_quiet_ = false;
    walls_[index].ws_id = id;
    steam_proc_->unsubscribe(id);
}

// Stop a download going. Steam is left subscribed by then, so the item goes
// exactly as an unsubscribe would (whatever arrived of it, deleted).
void WallPicker::ws_cancel(int index) {
    if (index < 0 || index >= static_cast<int>(walls_.size())) return;
    Wallpaper& w = walls_[index];
    if (w.ws_id.empty() || w.progress < 0 || !steam_proc_ || !steam_proc_->running()) {
        request();
        return;
    }
    ws_quiet_ = false;
    ws_cancelled_.insert(w.ws_id);
    if (ws_apply_after_ == w.ws_id) ws_apply_after_.clear();
    w.progress = -1;
    w.subscribed = w.installed = false;
    steam_proc_->cancel(w.ws_id);
    if (on_toast) on_toast("Descarga cancelada");
    request();
}

std::string WallPicker::ws_item_id(const Wallpaper& w) const {
    std::string id = !w.ws_id.empty() ? w.ws_id : (w.engine ? base_name(w.path) : "");
    return !id.empty() && id.find_first_not_of("0123456789") == std::string::npos ? id : "";
}

std::string WallPicker::ws_author(const Wallpaper& w) const {
    auto o = ws_owner_of_.find(ws_item_id(w));
    if (o == ws_owner_of_.end()) return "";
    auto n = ws_names_.find(o->second);
    return n == ws_names_.end() ? "" : n->second;
}

// The downloads come from disk, and project.json does not say who made them:
// Steam does, once per visit, for all of them at once (one helper started).
void WallPicker::ws_ask_authors() {
    if (ws_details_asked_ || ws_browse_) return;
    std::vector<std::string> ids;
    for (const auto& w : walls_) {
        std::string id = ws_item_id(w);
        if (!id.empty() && !ws_owner_of_.count(id)) ids.push_back(id);
    }
    if (ids.empty()) return;
    ws_details_asked_ = true;
    bool was = steam_proc_ && steam_proc_->running();
    if (!ws_helper()) return;
    if (!was) ws_quiet_ = true;  // only for this: Steam closed is no news
    steam_proc_->details(ids);
}

// The Workshop's previews, fetched to disk in parallel (the grid reads files).
void WallPicker::ws_thumbs(int gen) {
    std::string dir = cache_dir() + "/workshop";
    make_dirs(dir);
    auto jobs = std::make_shared<std::vector<std::pair<std::string, std::string>>>();
    for (auto& w : walls_)
        if (starts_with(w.thumb, "http")) jobs->push_back({w.thumb, dir + "/" + w.ws_id + ".img"});
    if (jobs->empty()) return;
    auto next = std::make_shared<std::atomic<size_t>>(0);
    for (int t = 0; t < 6; ++t)
        std::thread([this, jobs, next, gen] {
            for (size_t i; (i = (*next)++) < jobs->size();) {
                if (Loop::shutting_down()) return;
                auto [url, file] = (*jobs)[i];
                if (!file_exists(file) && !http_download(url, file, 20)) continue;
                Loop::post_if_alive([this, gen, url, file] {
                    if (gen != ws_gen_) return;
                    for (auto& w : walls_)
                        if (w.thumb == url) w.thumb = w.preview = file;
                    request();
                });
            }
        }).detach();
}

// The search box: wallhaven's and the Steam source's. Text goes in at the
// caret; the arrows move it while there is text (with nothing typed they are
// the pictures'); Ctrl+V pastes. Space is a word break; with nothing typed it
// is nobody's in the grids (not "apply": that would download the first card)
// and applies in the downloads, as in the folders.
bool WallPicker::search_key(TextField& f, xkb_keysym_t sym, const std::string& utf8, uint32_t mods,
                            bool space_applies) {
    bool ctrl = mods & Wayland::MOD_CTRL;
    bool has = !f.text.empty();
    auto moved = [this] {
        request();
        return true;
    };
    auto edited = [this] {
        search_edited();
        return true;
    };
    if (ctrl && (sym == XKB_KEY_v || sym == XKB_KEY_V)) {
        if (!proc::has("wl-paste")) return true;
        auto out = std::make_shared<std::string>();
        bool steam = steam_, browse = ws_browse_, remote = remote_;
        Loop::get().async([out] { *out = proc::run({"wl-paste", "--no-newline", "--type", "text/plain"}, 2000).out; },
                          [this, out, &f, steam, browse, remote] {
                              // still the same box (the source may have changed meanwhile)
                              if (out->empty() || steam != steam_ || browse != ws_browse_ || remote != remote_) return;
                              f.insert(*out);
                              search_edited();
                          });
        return true;
    }
    switch (sym) {
        case XKB_KEY_BackSpace: return has && f.backspace(ctrl) ? edited() : false;
        case XKB_KEY_Delete: return has && f.del() ? edited() : false;
        case XKB_KEY_Escape:
            if (!has) return false;
            f.clear();
            return edited();
        case XKB_KEY_Left:
            if (!has) return false;
            f.left(ctrl);
            return moved();
        case XKB_KEY_Right:
            if (!has) return false;
            f.right(ctrl);
            return moved();
        case XKB_KEY_Home:
            if (!has) return false;
            f.home();
            return moved();
        case XKB_KEY_End:
            if (!has) return false;
            f.end();
            return moved();
        case XKB_KEY_space:
            if (has) {
                f.insert(" ");
                return edited();
            }
            return !space_applies;
        default: break;
    }
    if (ctrl || utf8.empty() || static_cast<unsigned char>(utf8[0]) < 0x20 || utf8[0] == 0x7f) return false;
    f.insert(utf8);
    return edited();
}

void WallPicker::search_edited() {
    if (remote_) {
        Loop::get().cancel(wh_timer_);
        wh_timer_ = Loop::get().after(0.55, [this] {
            wh_timer_ = 0;
            if (remote_) wh_fetch(false);
        });
    } else if (steam_ && ws_browse_) {
        Loop::get().cancel(ws_timer_);
        ws_timer_ = Loop::get().after(0.55, [this] {
            ws_timer_ = 0;
            if (steam_ && ws_browse_) ws_fetch(false);
        });
    } else if (steam_) {
        refilter();
    }
    request();
}

// ── the side panel ────────────────────────────────────────────────────────

void WallPicker::free_anim() {
    for (auto* s : anim_) cairo_surface_destroy(s);
    anim_.clear();
    anim_ms_.clear();
    anim_key_.clear();
}

void WallPicker::open_panel(int vis) {
    if (vis < 0 || vis >= static_cast<int>(visible_.size())) return;
    int index = visible_[vis];
    panel_ = true;
    panel_t_.set(1);
    if (panel_index_ == index) return;
    panel_index_ = index;
    free_anim();
    const Wallpaper& w = walls_[index];
    panel_props_.clear();
    panel_values_ = nlohmann::json::object();
    if (steam_ && !w.remote && w.engine) {
        panel_props_ = workshop::properties(w.path);
        panel_values_ = workshop::user_values(w.path);
    }
    if (steam_ && !ws_item_id(w).empty() && !ws_owner_of_.count(ws_item_id(w))) ws_ask_authors();
    // wallhaven: the full picture, for the panel, for trying and keeping
    if (w.remote && !steam_) {
        std::string full = wh_full_path(w), url = w.path;
        if (!file_exists(full))
            std::thread([this, url, full] {
                make_dirs(full.substr(0, full.rfind('/')));
                if (http_download(url, full + ".part", 60)) rename((full + ".part").c_str(), full.c_str());
                Loop::post_if_alive([this] { request(); });
            }).detach();
    }
    // The Workshop: its preview moving (a small GIF, but the real motion). The
    // downloads keep the engine's sharp still instead.
    std::string src = !w.preview.empty() && !starts_with(w.preview, "http") ? w.preview : "";
    if (steam_ && w.remote && !src.empty()) {
        anim_key_ = src;
        int pw = static_cast<int>(kPanelW - 32) * std::max(1, scale()), ph = pw * 9 / 16;
        auto frames = std::make_shared<std::vector<cairo_surface_t*>>();
        auto ms = std::make_shared<std::vector<int>>();
        Loop::get().async([src, pw, ph, frames, ms] { load_frames(src, pw, ph, 120, *frames, *ms); },
                          [this, src, frames, ms] {
                              if (anim_key_ != src || !anim_.empty()) {
                                  for (auto* s : *frames) cairo_surface_destroy(s);
                                  return;
                              }
                              int sc = std::max(1, scale());
                              for (auto* s : *frames) cairo_surface_set_device_scale(s, sc, sc);
                              anim_ = std::move(*frames);
                              anim_ms_ = std::move(*ms);
                              anim_t0_ = now_mono();
                              request();
                          });
    }
    request();
}

void WallPicker::close_panel() {
    panel_ = false;
    panel_t_.set(0);
    panel_index_ = -1;
    free_anim();
    request();
}

std::string WallPicker::wh_full_path(const Wallpaper& w) const {
    std::string ext = w.path.substr(w.path.rfind('.') == std::string::npos ? w.path.size() : w.path.rfind('.'));
    if (ext.size() > 5 || ext.empty()) ext = ".jpg";
    return cache_dir() + "/wallhaven/full-" + w.name + ext;
}

// The card's main action: get it (and apply) if it is not here, apply if it is.
void WallPicker::panel_primary(int index) {
    if (index < 0 || index >= static_cast<int>(walls_.size())) return;
    const Wallpaper& w = walls_[index];
    if (steam_ && w.remote) {
        ws_get(index, true);
        return;
    }
    for (size_t v = 0; v < visible_.size(); ++v)
        if (visible_[v] == index) sel_ = static_cast<int>(v);
    slices_close(true);
}

void WallPicker::draw_panel(cairo_t* cr, float W, float H, float fade) {
    const auto& p = T();
    float pt = panel_t_.value;
    if (pt <= 0.01f || panel_index_ < 0 || panel_index_ >= static_cast<int>(walls_.size())) return;
    const Wallpaper& w = walls_[panel_index_];
    float a = clamp01(pt) * fade;
    // as tall as what it holds: picture, title, chips, buttons
    std::string title = steam_ ? w.name : (w.resolution.empty() ? w.name : w.resolution);
    Text tt{ui(15, "SemiBold"), p.text.with_alpha(a), Align::Left, kPanelW - 36, 2, false, true};
    float pic_h = std::round((kPanelW - 32) * 9 / 16), title_h = measure(title, tt).h;
    std::string author = steam_ ? ws_author(w) : "";
    float author_h = author.empty() ? 0 : 22;
    float content = 16 + pic_h + 16 + title_h + author_h + 10 + 24 + 22 + properties_height() + 52 + 16;
    Rect box{W - kPanelW - 24 + (1 - pt) * 60, 128, kPanelW, std::min(content, H - 128 - 24)};
    (void)H;
    fill(cr, box, 22, p.island.with_alpha(0.92f * a));
    stroke(cr, box, 22, p.text.with_alpha(0.06f * a));
    ui_.add(ID("pn.bg"), box);  // clicks on it stay on it
    w::Ctx c{cr, ui_};

    // the picture: animated when the preview is, else the best still there is
    Rect pic{box.x + 16, box.y + 16, box.w - 32, std::round((box.w - 32) * 9 / 16)};
    cairo_save(cr);
    rrect(cr, pic, 14);
    cairo_clip(cr);
    fill(cr, pic, 0, p.text.with_alpha(0.05f * a));
    cairo_surface_t* img = nullptr;
    if (!anim_.empty()) {
        int total = 0;
        for (int ms : anim_ms_) total += ms;
        int t = static_cast<int>((now_mono() - anim_t0_) * 1000) % std::max(1, total), k = 0;
        while (k + 1 < static_cast<int>(anim_.size()) && t >= anim_ms_[k]) t -= anim_ms_[k++];
        img = anim_[k];
    } else if (!steam_ && w.remote && file_exists(wh_full_path(w))) {
        img = ImageCache::get().request(wh_full_path(w), static_cast<int>(pic.w * 2));
    }
    if (!img && !w.thumb.empty() && !starts_with(w.thumb, "http")) img = ImageCache::get().request(w.thumb, 520);
    if (img) cover_rect(cr, img, pic, a);
    cairo_restore(cr);
    if (w.progress >= 0) ring(cr, pic.cx(), pic.cy(), 22, w.progress, p.accent, a);

    // what it is: a title and a few chips
    float y = pic.bottom() + 16;
    text(cr, box.x + 18, y, title, tt);
    y += title_h;
    if (!author.empty()) {  // who made it
        Text at{ui(12, "Medium"), p.text_dim.with_alpha(a), Align::Left, kPanelW - 60, 1, false, true};
        glyph(cr, box.x + 25, y + 13, "󰀄", 12, p.text_dim.with_alpha(a));
        text(cr, box.x + 38, y + 5, "por " + author, at);
        y += author_h;
    }
    y += 10;
    float cx = box.x + 18;
    auto chip = [&](const char* g, const std::string& label, Color col) {
        Text tt{ui(11.5f, "Medium"), col.with_alpha(0.9f * a)};
        float tw = label.empty() ? 0 : measure(label, tt).w + 6;
        Rect r{cx, y, 28 + tw, 24};
        fill(cr, r, 12, p.text.with_alpha(0.07f * a));
        glyph(cr, r.x + 14, r.cy(), g, 12, col.with_alpha(0.9f * a));
        if (!label.empty()) text(cr, r.x + 24, r.y + 4, label, tt);
        cx += r.w + 6;
    };
    if (steam_) {
        if (!w.ws_type.empty()) chip(type_glyph(w.ws_type), "", p.text);
        if (w.ws_size) chip("󰋊", human_size(w.ws_size), p.text_dim);
        if (w.ws_subs) chip("󰋑", human_count(w.ws_subs), p.text_dim);
        if (w.installed) chip("󰄬", "", p.accent);
    } else if (w.remote) {
        if (file_exists(wh_full_path(w))) chip("󰄬", "", p.accent);
    }
    if (w.mature) chip("󰒘", "", Color{0.92f, 0.26f, 0.26f, 1});
    y += 24 + 14;
    if (!panel_props_.empty()) draw_properties(cr, {box.x + 18, y, box.w - 36, properties_height()}, a);

    // what to do: the main action big, the rest round
    float by = box.bottom() - 16 - 52;
    bool here = !w.remote;  // on disk: apply; else get first
    bool can_try = here || (!steam_ && w.remote);
    bool can_drop = steam_ && (w.subscribed || here);
    bool can_save = !steam_ && w.remote;
    int nsec = 1 + (can_try ? 1 : 0) + (can_drop || can_save ? 1 : 0);  // the round ones; open is always there
    Rect main{box.x + 16, by, box.w - 32 - nsec * 58, 52};
    const char* mg = steam_ ? (here ? "󰐊" : "󰇚") : "󰄬";
    if (w.progress >= 0) {
        // downloading: the bar fills; under the pointer it offers to stop
        uint64_t cid = ID("pn.cancel");
        float hv = ui_.hover(cid);
        Color red{0.92f, 0.36f, 0.36f, 1};
        fill(cr, main, 26, p.accent.with_alpha(0.25f * a));
        Rect bar{main.x, main.y, main.w * std::max(0.04f, w.progress), main.h};
        cairo_save(cr);
        rrect(cr, main, 26);
        cairo_clip(cr);
        fill(cr, bar, 0, p.accent.with_alpha(0.9f * a));
        fill(cr, main, 0, red.with_alpha(0.85f * hv * a));
        cairo_restore(cr);
        glyph(cr, main.cx(), main.cy(), "󰇚", 20, p.on_accent.with_alpha((1 - hv) * a));
        glyph(cr, main.cx(), main.cy(), "󰅖", 20, Color{1, 1, 1, hv * a});
        int index = panel_index_;
        ui_.add(cid, main).click([this, index] { ws_cancel(index); });
    } else {
        uint64_t mid = ID("pn.main");
        float hv = ui_.hover(mid);
        fill(cr, main, 26, p.accent.with_alpha((0.88f + 0.12f * hv) * a));
        glyph(cr, main.cx(), main.cy(), mg, 22, p.on_accent.with_alpha(a));
        int index = panel_index_;
        ui_.add(mid, main).click([this, index] { panel_primary(index); });
    }
    float rx = main.right() + 6;
    auto round_btn = [&](uint64_t id, const char* g, std::function<void()> fn, Color tint = {0, 0, 0, 0}) {
        Rect r{rx + 3, by + 3, 46, 46};
        w::icon_button(c, id, r, g, 18, false, tint).click(std::move(fn));
        rx += 58;
    };
    int index = panel_index_;
    if (can_try) round_btn(ID("pn.try"), "󰈈", [this, index] { try_on(index); });
    if (can_drop)
        round_btn(ID("pn.unsub"), "󰆴", [this, index] { ws_unsubscribe(index); }, Color{0.92f, 0.36f, 0.36f, 1});
    else if (can_save)  // wallhaven: keep it in the folder without applying
        round_btn(ID("pn.save"), "󰇚", [this, index] {
            Wallpaper wp = walls_[index];
            std::string dir = expand_home(config().wallhaven_dir), full = wh_full_path(wp);
            std::string dest = dir + "/wallhaven-" + wp.name + full.substr(full.rfind('.'));
            std::string url = wp.path;
            std::thread([this, dir, dest, full, url] {
                make_dirs(dir);
                bool ok = file_exists(dest) || (file_exists(full) ? proc::run({"cp", full, dest}).ok() : http_download(url, dest, 60));
                Loop::post_if_alive([this, ok] {
                    if (on_toast) on_toast(ok ? "Guardado" : "No se pudo guardar");
                    if (ok) scan();
                });
            }).detach();
        });
    std::string page = steam_ ? "https://steamcommunity.com/sharedfiles/filedetails/?id=" + (w.ws_id.empty() ? base_name(w.path) : w.ws_id)
                              : "https://wallhaven.cc/w/" + w.name;
    round_btn(ID("pn.open"), "󰏌", [page] { proc::spawn({"xdg-open", page}); });

    // close, top right over the picture
    Rect xr{pic.right() - 34, pic.y + 6, 28, 28};
    fill(cr, xr, 14, Color{0, 0, 0, 0.45f * a});
    glyph(cr, xr.cx(), xr.cy(), "󰅖", 13, Color{1, 1, 1, 0.9f * a});
    ui_.add(ID("pn.close"), xr).click([this] { close_panel(); });
}

// ── an item's own settings ────────────────────────────────────────────────

namespace {
constexpr float kPropRow = 38;
}

float WallPicker::properties_height() const {
    int rows = 0;
    for (auto& p : panel_props_)
        if (workshop::visible(p, panel_props_, panel_values_)) ++rows;
    return rows ? rows * kPropRow + 4 : 0;
}

void WallPicker::set_property(const workshop::Property& p, const nlohmann::json& value) {
    if (panel_index_ < 0 || panel_index_ >= static_cast<int>(walls_.size())) return;
    std::string path = walls_[panel_index_].path;
    panel_values_[p.name] = value;
    workshop::set_user_value(path, p.name, value);
    // on screen: the engine takes it (live where the scene allows), a moment
    // after the last change so a slider does not reload it at every step
    if (path != current_) return;
    Loop::get().cancel(props_timer_);
    props_timer_ = Loop::get().after(0.3, [this, path] {
        props_timer_ = 0;
        auto ok = std::make_shared<bool>(false);
        std::string props = workshop::engine_overrides(path);
        Loop::get().async([props, ok] { *ok = engine::set_properties(props); },
                          [this, path, ok] {
                              if (!*ok && on_apply) on_apply(path);  // not running it: apply it whole
                          });
    });
    request();
}

float WallPicker::draw_properties(cairo_t* cr, const Rect& area, float a) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    float y = area.y;
    int n = 0;
    for (auto& prop : panel_props_) {
        if (!workshop::visible(prop, panel_props_, panel_values_)) continue;
        uint64_t id = ID("pn.prop", n++);
        nlohmann::json v = workshop::value_of(prop, panel_values_);
        Text label{ui(12, "Medium"), p.text_dim.with_alpha(a), Align::Left, 130};
        text(cr, area.x, y + kPropRow / 2 - 8, prop.text, label);
        Rect ctl{area.x + 138, y + 6, area.w - 138, kPropRow - 12};
        if (prop.type == "bool") {
            bool on = v.is_boolean() ? v.get<bool>() : (v.is_number() && v.get<double>() != 0);
            w::toggle(c, id, {ctl.right() - 44, ctl.cy() - 12, 44, 24}, on, [this, prop, on] { set_property(prop, !on); });
        } else if (prop.type == "slider") {
            double val = v.is_number() ? v.get<double>() : prop.min;
            double span = std::max(1e-9, prop.max - prop.min);
            bool whole = prop.step >= 1 || (prop.step == 0 && prop.max - prop.min > 10);
            char buf[32];
            snprintf(buf, sizeof(buf), whole ? "%.0f" : "%.1f", val);
            w::slider(c, id, ctl, static_cast<float>((val - prop.min) / span), nullptr,
                      [this, prop, span, whole](float f) {
                          double x = prop.min + f * span;
                          if (prop.step > 0) x = prop.min + std::round((x - prop.min) / prop.step) * prop.step;
                          if (whole) x = std::round(x);
                          set_property(prop, x);
                      },
                      {}, buf);
        } else if (prop.type == "combo") {
            size_t at = 0;
            for (size_t i = 0; i < prop.options.size(); ++i)
                if (prop.options[i].second == v) at = i;
            std::string shown = prop.options[at].first.empty() ? (prop.options[at].second.is_string() ? prop.options[at].second.get<std::string>() : prop.options[at].second.dump()) : prop.options[at].first;
            size_t count = prop.options.size();
            w::pill(c, id, ctl, shown, nullptr, w::Style::Tonal, 12)
                .click([this, prop, at, count] { set_property(prop, prop.options[(at + 1) % count].second); })
                .right([this, prop, at, count] { set_property(prop, prop.options[(at + count - 1) % count].second); });
        } else if (prop.type == "color" && v.is_string()) {
            float r = 0, g = 0, b = 0;
            sscanf(v.get<std::string>().c_str(), "%f %f %f", &r, &g, &b);
            fill(cr, {ctl.right() - 44, ctl.cy() - 11, 44, 22}, 11, Color{r, g, b, a});
            stroke(cr, {ctl.right() - 44, ctl.cy() - 11, 44, 22}, 11, p.text.with_alpha(0.2f * a));
        }
        y += kPropRow;
    }
    return y - area.y;
}

// ── trying it on the desktop ──────────────────────────────────────────────

void WallPicker::try_on(int index) {
    if (index < 0 || index >= static_cast<int>(walls_.size()) || trying_) return;
    const Wallpaper& w = walls_[index];
    try_prev_ = current_.empty() ? current_wallpaper() : current_;
    try_index_ = index;
    trying_ = true;
    try_t_.set(1);
    if (!w.remote) {
        try_path_ = w.path;
        if (on_apply) on_apply(try_path_);
    } else if (!steam_) {
        // wallhaven: the full picture first (the panel may have it already)
        std::string full = wh_full_path(w), url = w.path;
        try_path_ = full;
        try_busy_ = true;
        std::thread([this, url, full] {
            make_dirs(full.substr(0, full.rfind('/')));
            bool ok = file_exists(full) || http_download(url, full, 60);
            Loop::post_if_alive([this, ok, full] {
                try_busy_ = false;
                if (!trying_ || try_path_ != full) return;
                if (!ok) {
                    if (on_toast) on_toast("No se pudo descargar de wallhaven");
                    try_end(false);
                    return;
                }
                if (on_apply) on_apply(full);
                request();
            });
        }).detach();
    }
    request();
}

void WallPicker::try_end(bool keep) {
    if (!trying_) return;
    trying_ = false;
    try_t_.set(0);
    if (!keep) {
        if (!try_busy_ && !try_prev_.empty() && on_apply) on_apply(try_prev_);
        try_busy_ = false;
        request();
        return;
    }
    if (try_busy_) return;  // nothing on screen yet to keep
    std::string kept = try_path_;
    if (!steam_ && try_index_ >= 0 && try_index_ < static_cast<int>(walls_.size()) && walls_[try_index_].remote) {
        // wallhaven: into the folder, an ordinary wallpaper from now on
        const Wallpaper& w = walls_[try_index_];
        std::string dir = expand_home(config().wallhaven_dir);
        kept = dir + "/wallhaven-" + w.name + try_path_.substr(try_path_.rfind('.'));
        make_dirs(dir);
        if (!file_exists(kept)) proc::run({"cp", try_path_, kept});
        if (on_apply) on_apply(kept);
    }
    applied_path_ = current_ = kept;
    leaving_ = true;  // done: back into the island
    leave_t0_ = now_mono();
    request();
}

// While trying: nothing but two buttons at the bottom over the real desktop.
void WallPicker::draw_try(cairo_t* cr, float W, float H) {
    const auto& p = T();
    float a = clamp01(try_t_.value);
    if (a <= 0.01f) return;
    Rect cap{W / 2 - 66, H - 96 + (1 - a) * 40, 132, 60};
    fill(cr, cap, 30, p.island.with_alpha(0.94f * a));
    w::Ctx c{cr, ui_};
    Rect no{cap.x + 8, cap.y + 8, 44, 44}, yes{cap.right() - 52, cap.y + 8, 44, 44};
    w::icon_button(c, ID("try.no"), no, "󰅖", 18).click([this] { try_end(false); });
    if (try_busy_) {
        ring(cr, yes.cx(), yes.cy(), 12, -1, p.accent, a);
    } else {
        float hv = ui_.hover(ID("try.yes"));
        fill(cr, yes, 22, p.accent.with_alpha((0.88f + 0.12f * hv) * a));
        glyph(cr, yes.cx(), yes.cy(), "󰄬", 20, p.on_accent.with_alpha(a));
        ui_.add(ID("try.yes"), yes).click([this] { try_end(true); });
    }
}

}  // namespace island
