// The wallpaper picker's default look, after skwd-wall: a row of skewed
// slices over the dimmed desktop, the selected one large, the rest small, all
// backlit in the theme's accent. It pours out of the resting island and goes
// back into it; applying straightens the chosen slice until it is the screen.
//
// Nothing heavy happens on the main thread: slice pictures, lights, shapes
// and the veil are made by workers and kept between openings.

#include <atomic>
#include <cmath>
#include <thread>
#include <unistd.h>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "services/images.hpp"
#include "suite/wallpicker.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

#include <nlohmann/json.hpp>

namespace island {

using namespace paint;

namespace {

float ease_io(float t) {
    t = clamp01(t);
    return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}
float ease_out(float t) {
    t = clamp01(t);
    return 1 - std::pow(1 - t, 3);
}
float ease_out_back(float t) {
    t = clamp01(t);
    const float c1 = 1.25f, c3 = c1 + 1;
    float u = t - 1;
    return 1 + c3 * u * u * u + c1 * u * u;
}
float ease_in(float t) {
    t = clamp01(t);
    return t * t * t;
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

// A parallelogram whose top leans `sk` to the right, corners softened.
void quad_path(cairo_t* cr, float x, float y, float w, float h, float sk, float r) {
    r = std::min({r, w / 3, h / 3});
    float k = h > 0 ? sk / h : 0;
    float x0 = x + sk, x1 = x + sk + w, x2 = x + w, x3 = x;
    float y0 = y, y1 = y + h;
    cairo_new_path(cr);
    cairo_move_to(cr, x0 + r, y0);
    cairo_line_to(cr, x1 - r, y0);
    cairo_curve_to(cr, x1, y0, x1, y0, x1 - r * k, y0 + r);
    cairo_line_to(cr, x2 + r * k, y1 - r);
    cairo_curve_to(cr, x2, y1, x2, y1, x2 - r, y1);
    cairo_line_to(cr, x3 + r, y1);
    cairo_curve_to(cr, x3, y1, x3, y1, x3 + r * k, y1 - r);
    cairo_line_to(cr, x0 - r * k, y0 + r);
    cairo_curve_to(cr, x0, y0, x0, y0, x0 + r, y0);
    cairo_close_path(cr);
}

void cover(cairo_t* cr, cairo_surface_t* img, const Rect& r, float alpha, cairo_filter_t f) {
    int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) return;
    double s = std::max(r.w / iw, r.h / ih);
    cairo_save(cr);
    cairo_translate(cr, r.x + (r.w - iw * s) / 2, r.y + (r.h - ih * s) / 2);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), f);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

cairo_surface_t* fitted(cairo_surface_t* img, int w, int h) {
    if (!img || w <= 0 || h <= 0) return nullptr;
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* c = cairo_create(out);
    cover(c, img, {0, 0, static_cast<float>(w), static_cast<float>(h)}, 1, CAIRO_FILTER_GOOD);
    cairo_destroy(c);
    return out;
}

void blur_a8(unsigned char* px, int w, int h, int stride, int radius) {
    std::vector<unsigned char> tmp(static_cast<size_t>(std::max(w, h)));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            unsigned char* row = px + y * stride;
            int sum = 0, n = 2 * radius + 1;
            for (int k = -radius; k <= radius; ++k) sum += row[std::clamp(k, 0, w - 1)];
            for (int x = 0; x < w; ++x) {
                tmp[x] = static_cast<unsigned char>(sum / n);
                sum += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
            }
            std::copy(tmp.begin(), tmp.begin() + w, row);
        }
        for (int x = 0; x < w; ++x) {
            int sum = 0, n = 2 * radius + 1;
            for (int k = -radius; k <= radius; ++k) sum += px[std::clamp(k, 0, h - 1) * stride + x];
            for (int y = 0; y < h; ++y) {
                tmp[y] = static_cast<unsigned char>(sum / n);
                sum += px[std::min(y + radius + 1, h - 1) * stride + x] - px[std::max(y - radius, 0) * stride + x];
            }
            for (int y = 0; y < h; ++y) px[y * stride + x] = tmp[y];
        }
    }
}

constexpr float kGlowPad = 44;

std::string size_key(float w, float h, float sk) {
    return std::to_string(static_cast<int>(w)) + "x" + std::to_string(static_cast<int>(h)) + "s" +
           std::to_string(static_cast<int>(sk));
}

// Layout of the row for a screen.
struct Geo {
    float big_w, big_h, small_w, small_h, gap, cy;
    float lean(float h) const { return std::round(h * 0.16f); }
};
Geo geo_for(float W, float H) {
    Geo g;
    g.big_h = std::round(H * 0.50f);
    g.big_w = std::round(W * 0.27f);
    g.small_h = std::round(g.big_h * 0.66f);
    g.small_w = std::round(W * 0.036f);
    g.gap = std::round(W * 0.006f);
    g.cy = std::round(H * 0.53f);
    return g;
}

float island_y() { return (config().island_style == "attached" ? 0 : config().margin_top) + config().bar_height / 2.0f; }

}  // namespace

// ── caches made by workers ────────────────────────────────────────────────

void WallPicker::free_caches() {
    if (big_fit_) {
        cairo_surface_destroy(big_fit_);
        big_fit_ = nullptr;
    }
    big_path_.clear();
    for (auto* m : {&slice_cache_, &glow_cache_, &shape_cache_}) {
        for (auto& [k, v] : *m)
            if (v) cairo_surface_destroy(v);
        m->clear();
    }
    if (veil_) cairo_surface_destroy(veil_);
    veil_ = nullptr;
}

void WallPicker::make_async(const std::string& key, std::function<cairo_surface_t*()> work,
                            std::unordered_map<std::string, cairo_surface_t*>* into) {
    if (pending_.count(key) || inflight_ >= 8) return;
    pending_[key] = true;
    ++inflight_;
    auto out = std::make_shared<cairo_surface_t*>(nullptr);
    Loop::get().async([work, out] { *out = work(); },
                      [this, key, out, into] {
                          pending_.erase(key);
                          --inflight_;
                          if (!*out) return;
                          if (into == &slice_cache_ && into->size() > 140) {
                              for (auto& [k, v] : *into) cairo_surface_destroy(v);
                              into->clear();
                          }
                          auto it = into->find(key);
                          if (it != into->end() && it->second) cairo_surface_destroy(it->second);
                          (*into)[key] = *out;
                          request();
                      });
}

cairo_surface_t* WallPicker::slice_image(const std::string& thumb, int w, int h, float sk) {
    if (starts_with(thumb, "http")) return nullptr;  // still downloading (wallhaven)
    int sc = std::max(1, scale());
    std::string key = thumb + "@" + size_key(static_cast<float>(w), static_cast<float>(h), sk) + "@" + std::to_string(sc);
    auto it = slice_cache_.find(key);
    if (it != slice_cache_.end()) return it->second;
    make_async(key,
               [thumb, w, h, sk, sc]() -> cairo_surface_t* {
                   cairo_surface_t* src = load_image_blocking(thumb, 520);  // local files only (see below)
                   if (!src) return nullptr;
                   cairo_surface_t* out = fitted(src, w * sc, h * sc);
                   cairo_surface_destroy(src);
                   if (!out) return nullptr;
                   if (sk >= 0) {  // cut to shape once: drawing it needs no clip
                       cairo_t* c = cairo_create(out);
                       cairo_scale(c, sc, sc);
                       cairo_set_operator(c, CAIRO_OPERATOR_DEST_IN);
                       quad_path(c, 0, 0, w - sk, h, sk, 12);
                       cairo_set_source_rgba(c, 0, 0, 0, 1);
                       cairo_fill(c);
                       cairo_destroy(c);
                   }
                   cairo_surface_set_device_scale(out, sc, sc);
                   return out;
               },
               &slice_cache_);
    return nullptr;
}

cairo_surface_t* WallPicker::glow_image(float w, float h, float sk, Color col) {
    char ck[16];
    snprintf(ck, sizeof(ck), "%02x%02x%02x", static_cast<int>(col.r * 255), static_cast<int>(col.g * 255),
             static_cast<int>(col.b * 255));
    std::string key = "glow" + size_key(w, h, sk) + ck;
    auto it = glow_cache_.find(key);
    if (it != glow_cache_.end()) return it->second;
    make_async(key,
               [w, h, sk, col]() -> cairo_surface_t* {
                   int pw = static_cast<int>(w + sk + 2 * kGlowPad), ph = static_cast<int>(h + 2 * kGlowPad);
                   cairo_surface_t* m = cairo_image_surface_create(CAIRO_FORMAT_A8, pw, ph);
                   cairo_t* c = cairo_create(m);
                   quad_path(c, kGlowPad, kGlowPad, w, h, sk, 12);
                   cairo_set_source_rgba(c, 0, 0, 0, 1);
                   cairo_set_line_width(c, 8);
                   cairo_stroke_preserve(c);
                   cairo_fill(c);
                   cairo_destroy(c);
                   cairo_surface_flush(m);
                   blur_a8(cairo_image_surface_get_data(m), pw, ph, cairo_image_surface_get_stride(m), 13);
                   cairo_surface_mark_dirty(m);
                   cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
                   cairo_t* oc = cairo_create(out);
                   cairo_set_source_rgba(oc, col.r, col.g, col.b, 0.9);
                   cairo_mask_surface(oc, m, 0, 0);
                   cairo_destroy(oc);
                   cairo_surface_destroy(m);
                   return out;
               },
               &glow_cache_);
    return nullptr;
}

cairo_surface_t* WallPicker::shape_image(float w, float h, float sk) {
    int sc = std::max(1, scale());
    std::string key = "shape" + size_key(w, h, sk) + "@" + std::to_string(sc);
    auto it = shape_cache_.find(key);
    if (it != shape_cache_.end()) return it->second;
    make_async(key,
               [w, h, sk, sc]() -> cairo_surface_t* {
                   cairo_surface_t* m = cairo_image_surface_create(CAIRO_FORMAT_A8, static_cast<int>(w + sk) * sc,
                                                                   static_cast<int>(h) * sc);
                   cairo_t* c = cairo_create(m);
                   cairo_scale(c, sc, sc);
                   quad_path(c, 0, 0, w, h, sk, 12);
                   cairo_set_source_rgba(c, 0, 0, 0, 1);
                   cairo_fill(c);
                   cairo_destroy(c);
                   cairo_surface_set_device_scale(m, sc, sc);
                   return m;
               },
               &shape_cache_);
    return nullptr;
}

// The selection at screen size, for the insertion.
void WallPicker::request_big() {
    if (sel_ < 0 || sel_ >= static_cast<int>(visible_.size()) || last_w_ <= 0) return;
    const Wallpaper& w = walls_[visible_[sel_]];
    if (w.remote) return;  // the full image comes when it is applied
    std::string src = w.video ? w.thumb : w.path;
    if (src.empty() || src == big_path_ || src == big_loading_) return;
    big_loading_ = src;
    int sc = std::max(1, scale());
    int bw = static_cast<int>(last_w_) * sc, bh = static_cast<int>(last_h_) * sc;
    auto out = std::make_shared<cairo_surface_t*>(nullptr);
    Loop::get().async(
        [src, bw, bh, out] {
            cairo_surface_t* img = load_image_blocking(src, std::max(bw, bh));
            *out = fitted(img, bw, bh);
            if (img) cairo_surface_destroy(img);
        },
        [this, src, out, sc] {
            if (big_loading_ == src) big_loading_.clear();
            if (!*out) return;
            cairo_surface_set_device_scale(*out, sc, sc);
            if (big_fit_) cairo_surface_destroy(big_fit_);
            big_fit_ = *out;
            big_path_ = src;
            request();
        });
}

// Before the first opening: the list, the lights, and the slices round the current one.
void WallPicker::prewarm() {
    current_ = current_wallpaper();
    // wallhaven's hosts resolved ahead (a slow resolver costs seconds per host)
    std::thread([] {
        std::string b;
        http_get("https://wallhaven.cc/api/v1/search?page=1", b, 15);
        http_get("https://th.wallhaven.cc/", b, 10);
    }).detach();
    if (walls_.empty()) scan();
    auto size = std::make_shared<std::pair<int, int>>(0, 0);
    Loop::get().async(
        [size] {
            auto r = proc::run({"niri", "msg", "-j", "focused-output"}, 3000);
            auto j = nlohmann::json::parse(r.out, nullptr, false);
            if (!j.is_object() || !j.contains("logical")) return;
            auto& l = j["logical"];
            size->first = l.value("width", 0);
            size->second = l.value("height", 0);
        },
        [this, size] {
            if (size->first <= 0) return;
            float W = static_cast<float>(size->first), H = static_cast<float>(size->second);
            last_w_ = W;
            last_h_ = H;
            Geo g = geo_for(W, H);
            glow_image(g.small_w, g.small_h, g.lean(g.small_h), T().accent);
            glow_image(g.big_w, g.big_h, g.lean(g.big_h), T().accent);
            shape_image(g.small_w, g.small_h, g.lean(g.small_h));
            // the slices once the list is there; a few rounds, as room frees up
            auto step = std::make_shared<std::function<void(int)>>();
            *step = [this, g, step](int round) {
                if (round > 6 || exists()) return;
                refilter();
                int c = 0;
                for (size_t i = 0; i < visible_.size(); ++i)
                    if (walls_[visible_[i]].path == current_) c = static_cast<int>(i);
                for (int d = 0; d <= 14; ++d)
                    for (int s : {c - d, c + d}) {
                        if (s < 0 || s >= static_cast<int>(visible_.size()) || walls_[visible_[s]].thumb.empty()) continue;
                        const std::string& th = walls_[visible_[s]].thumb;
                        if (d == 0) {
                            slice_image(th, static_cast<int>(g.big_w + g.lean(g.big_h)), static_cast<int>(g.big_h), g.lean(g.big_h));
                            slice_image(th, static_cast<int>(g.big_w + g.lean(g.big_h)), static_cast<int>(g.big_h), -1);
                        } else {
                            slice_image(th, static_cast<int>(g.small_w + g.lean(g.small_h)), static_cast<int>(g.small_h),
                                        g.lean(g.small_h));
                        }
                    }
                Loop::get().after(0.8, [step, round] { (*step)(round + 1); });
            };
            Loop::get().after(1.0, [step] { (*step)(0); });
        });
}

void WallPicker::backdrop_shown(const std::string& path) {
    if (!applied_path_.empty() && path == applied_path_) {
        backdrop_ready_ = true;
        request();
    }
}

// ── wallhaven ─────────────────────────────────────────────────────────────

namespace {
std::string url_encode(const std::string& v) {
    std::string o;
    for (unsigned char ch : v) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o += static_cast<char>(ch);
        else if (ch == ' ') o += '+';
        else {
            char b[4];
            snprintf(b, sizeof(b), "%%%02X", ch);
            o += b;
        }
    }
    return o;
}
// wallhaven only takes colours from its own palette
const char* kWhColors[13] = {"cc0000", "ff6600", "ffcc33", "cccc33", "77cc33", "336600", "66cccc",
                             "0099cc", "0066cc", "333399", "993399", "ea4c88", "999999"};
}  // namespace

void WallPicker::set_remote(bool on) {
    if (on == remote_ || inserting_ || leaving_) return;
    remote_ = on;
    if (on) {
        local_walls_ = std::move(walls_);
        walls_.clear();
        visible_.clear();
        sel_ = 0;
        wh_fetch(false);
    } else {
        ++wh_gen_;  // drop answers still on their way
        wh_loading_ = false;
        walls_ = std::move(local_walls_);
        local_walls_.clear();
        refilter();
        sel_ = 0;
        for (size_t i = 0; i < visible_.size(); ++i)
            if (walls_[visible_[i]].path == current_) sel_ = static_cast<int>(i);
    }
    pos_.reset(static_cast<float>(std::max(0, sel_)));
    filter_t0_ = now_mono();
    request();
}

void WallPicker::wh_fetch(bool more) {
    if (more && (wh_loading_ || wh_page_ >= wh_last_page_)) return;
    int page = more ? wh_page_ + 1 : 1;
    if (!more) {
        ++wh_gen_;
        grid_scroll_.reset(0);
        grid_t0_ = now_mono();
        walls_.clear();
        visible_.clear();
        sel_ = 0;
        pos_.reset(0);
        if (wh_sort_ == 2) wh_seed_.clear();
    }
    wh_loading_ = true;
    wh_failed_ = false;
    static const char* sorts[4] = {"toplist", "date_added", "random", "views"};
    const auto& cfg = config();
    static const char* purity[3] = {"100", "110", "111"};
    std::string url = std::string("https://wallhaven.cc/api/v1/search?categories=110&purity=") + purity[wh_mature_] +
                      "&sorting=" + sorts[wh_sort_] + "&order=desc&topRange=1M&page=" + std::to_string(page);
    if (last_w_ > 0)
        url += "&atleast=" + std::to_string(static_cast<int>(last_w_ * std::max(1, scale()))) + "x" +
               std::to_string(static_cast<int>(last_h_ * std::max(1, scale())));
    if (!wh_query_.empty()) url += "&q=" + url_encode(wh_query_);
    if (hue_ >= 0 && hue_ < 13) url += std::string("&colors=") + kWhColors[hue_];
    if (wh_sort_ == 2 && !wh_seed_.empty()) url += "&seed=" + wh_seed_;
    if (!cfg.wallhaven_key.empty()) url += "&apikey=" + cfg.wallhaven_key;
    int gen = wh_gen_;
    auto body = std::make_shared<std::string>();
    auto ok = std::make_shared<bool>(false);
    // On its own thread: the shared workers may be busy making slices.
    auto done = [this, gen, page, body, ok] {
                          if (gen != wh_gen_ || !remote_) return;
                          wh_loading_ = false;
                          auto j = nlohmann::json::parse(*body, nullptr, false);
                          if (!*ok || !j.is_object() || !j.contains("data")) {
                              wh_failed_ = true;
                              request();
                              return;
                          }
                          wh_page_ = page;
                          if (j.contains("meta") && j["meta"].is_object()) {
                              wh_last_page_ = j["meta"].value("last_page", page);
                              if (j["meta"].contains("seed") && j["meta"]["seed"].is_string())
                                  wh_seed_ = j["meta"]["seed"].get<std::string>();
                          }
                          for (auto& d : j["data"]) {
                              if (!d.is_object()) continue;
                              Wallpaper w;
                              w.remote = true;
                              w.name = d.value("id", "");
                              w.path = d.value("path", "");
                              w.resolution = d.value("resolution", "");
                              w.mature = d.value("purity", std::string("sfw")) != "sfw";
                              if (d.contains("thumbs") && d["thumbs"].is_object()) w.thumb = d["thumbs"].value("large", "");
                              if (w.path.empty() || w.thumb.empty()) continue;
                              walls_.push_back(std::move(w));
                              visible_.push_back(static_cast<int>(walls_.size()) - 1);
                          }
                          wh_thumbs(gen);
                          request();
                      };
    std::thread([url, body, ok, done] {
        *ok = http_get(url, *body, 15);
        Loop::get().post(done);
    }).detach();
    request();
}

// The previews of what came in, fetched to disk in parallel; the carousel
// only ever reads local files.
void WallPicker::wh_thumbs(int gen) {
    std::string dir = cache_dir() + "/wallhaven";
    make_dirs(dir);
    auto jobs = std::make_shared<std::vector<std::pair<std::string, std::string>>>();  // url, file
    for (auto& w : walls_) {
        if (!w.remote || !starts_with(w.thumb, "http")) continue;
        std::string file = dir + "/" + w.name + ".jpg";
        jobs->push_back({w.thumb, file});
    }
    if (jobs->empty()) return;
    auto next = std::make_shared<std::atomic<size_t>>(0);
    for (int t = 0; t < 6; ++t) {
        std::thread([this, jobs, next, gen] {
            for (size_t i; (i = (*next)++) < jobs->size();) {
                auto [url, file] = (*jobs)[i];
                bool ok = file_exists(file) || http_download(url, file, 20);
                if (!ok) continue;
                Loop::get().post([this, gen, url, file] {
                    if (gen != wh_gen_) return;
                    for (auto& w : walls_)
                        if (w.remote && w.thumb == url) w.thumb = file;
                    request();
                });
            }
        }).detach();
    }
}

// Download it into the wallhaven folder, then it is an ordinary wallpaper.
void WallPicker::wh_apply(int index) {
    Wallpaper w = walls_[index];
    std::string dir = expand_home(config().wallhaven_dir);
    std::string ext = w.path.substr(w.path.rfind('.') == std::string::npos ? w.path.size() : w.path.rfind('.'));
    if (ext.size() > 5 || ext.empty()) ext = ".jpg";
    std::string dest = dir + "/wallhaven-" + w.name + ext;
    downloading_ = true;
    auto ok = std::make_shared<bool>(false);
    std::string url = w.path;
    auto done = [this, index, dest, ok] {
            downloading_ = false;
            if (!*ok) {
                inserting_ = false;  // back to browsing
                if (on_toast) on_toast("No se pudo descargar de wallhaven");
                request();
                return;
            }
            if (index < static_cast<int>(walls_.size())) {
                walls_[index].remote = false;
                walls_[index].path = dest;
            }
            applied_path_ = dest;
            current_ = dest;
            if (on_apply) on_apply(dest);
            // the folders' list will pick it up on the next scan
        };
    std::thread([dir, dest, url, ok, done] {
        make_dirs(dir);
        *ok = file_exists(dest) || http_download(url, dest, 60);
        Loop::get().post(done);
    }).detach();
}

// ── input ─────────────────────────────────────────────────────────────────

void WallPicker::slices_move(int delta) {
    int n = static_cast<int>(visible_.size());
    if (n == 0 || inserting_ || leaving_) return;
    int next = std::clamp((sel_ < 0 ? 0 : sel_) + delta, 0, n - 1);
    if (next == sel_) return;
    sel_ = next;
    moved_ = true;
    pos_.set(static_cast<float>(sel_));
    if (remote_) {
        grid_reveal(sel_);
        if (sel_ > n - grid_cols_ * 2) wh_fetch(true);
    }
    request();
}

void WallPicker::set_filter(int kind, int hue) {
    if (kind == kind_ && hue == hue_) return;
    kind_ = kind;
    hue_ = hue;
    if (remote_) {
        wh_fetch(false);
        filter_t0_ = now_mono();
        return;
    }
    refilter();
    pos_.reset(static_cast<float>(std::max(0, sel_)));
    filter_t0_ = now_mono();
    request();
}

void WallPicker::slices_close(bool apply_it) {
    if (inserting_ || leaving_) return;
    if (!apply_it || sel_ < 0 || sel_ >= static_cast<int>(visible_.size())) {
        leaving_ = true;  // back into the island
        leave_t0_ = now_mono();
        request();
        return;
    }
    inserting_ = true;
    ins_t0_ = now_mono();
    ins_index_ = sel_;
    pos_.reset(static_cast<float>(sel_));
    if (walls_[visible_[sel_]].remote) wh_apply(visible_[sel_]);  // downloaded while it grows
    else apply(visible_[sel_], true);  // set now: by the time the slice fills the screen, it is there
    request();
}

void WallPicker::key_slices(xkb_keysym_t sym, const std::string& utf8) {
    if (remote_) {
        // typing searches wallhaven (after a short pause)
        bool edit = false;
        if (sym == XKB_KEY_BackSpace && !wh_query_.empty()) {
            wh_query_.erase(utf8_prev(wh_query_, wh_query_.size()));
            edit = true;
        } else if (sym == XKB_KEY_Escape && !wh_query_.empty()) {
            wh_query_.clear();
            edit = true;
        } else if (!utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20 && sym != XKB_KEY_space) {
            wh_query_ += utf8;
            edit = true;
        }
        if (edit) {
            Loop::get().cancel(wh_timer_);
            wh_timer_ = Loop::get().after(0.55, [this] {
                wh_timer_ = 0;
                if (remote_) wh_fetch(false);
            });
            request();
            return;
        }
    }
    switch (sym) {
        case XKB_KEY_Escape:
            if (remote_) set_remote(false);
            else slices_close(false);
            return;
        case XKB_KEY_Left: slices_move(-1); return;
        case XKB_KEY_Right: slices_move(1); return;
        case XKB_KEY_Up:
            if (remote_) slices_move(-grid_cols_);
            return;
        case XKB_KEY_Down:
            if (remote_) slices_move(grid_cols_);
            return;
        case XKB_KEY_h:
        case XKB_KEY_a: slices_move(-1); return;
        case XKB_KEY_l:
        case XKB_KEY_d: slices_move(1); return;
        case XKB_KEY_w:
        case XKB_KEY_W: set_remote(true); return;
        case XKB_KEY_Page_Up: slices_move(-5); return;
        case XKB_KEY_Page_Down: slices_move(5); return;
        case XKB_KEY_Home: slices_move(-100000); return;
        case XKB_KEY_End: slices_move(100000); return;
        case XKB_KEY_Tab:
            if (remote_) {
                wh_sort_ = (wh_sort_ + 1) % 4;
                wh_fetch(false);
            } else {
                set_filter((kind_ + 1) % 3, hue_);
            }
            return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_space: slices_close(true); return;
        default: return;
    }
}

void WallPicker::canvas_button(Button b, bool pressed, float, float) {
    if (!slices_ || !pressed) return;
    if (b == Button::Right || b == Button::Left) slices_close(false);  // a click off the slices
}

// ── drawing ───────────────────────────────────────────────────────────────

// Source (folders / wallhaven), kind or order, the search, and a palette of
// colours: a capsule that drops out of the island.
void WallPicker::draw_filter_bar(cairo_t* cr, float W, float H, float t_in) {
    (void)H;
    const auto& p = T();
    w::Ctx c{cr, ui_};
    if (t_in <= 0.01f) return;
    float k = ease_out_back(t_in);
    const int ncol = 13;
    float bh = 42, dot = 20, dgap = 8, btn = 34;
    float search_w = remote_ ? 210 : 0;
    float mature_w = remote_ ? 34 + 8 : 0;
    int nmid = remote_ ? 4 : 3;
    float bw = 12 + btn + 12 + 1 + 12 + nmid * btn + (nmid - 1) * 4 + mature_w + 12 + 1 + 12 + (remote_ ? search_w + 12 + 1 + 12 : 0) +
               ncol * dot + (ncol - 1) * dgap + 14;
    float by = 64;
    float iy = island_y();
    Rect bar{std::round(W / 2 - bw / 2), std::round(lerp(iy - bh / 2, by, k)), bw, bh};
    cairo_save(cr);
    cairo_translate(cr, W / 2, bar.cy());
    cairo_scale(cr, lerp(0.2f, 1.0f, std::min(1.0f, k)), lerp(0.7f, 1.0f, std::min(1.0f, k)));
    cairo_translate(cr, -W / 2, -bar.cy());
    float a = clamp01(t_in * 3);
    fill(cr, bar, bh / 2, p.island.with_alpha(0.9f * a));
    if (t_in < 0.6f) {
        cairo_restore(cr);
        return;
    }
    float ca = clamp01((t_in - 0.6f) / 0.3f);
    auto sep = [&](float& x) {
        x += 6;
        fill(cr, {x, bar.y + 11, 1, bh - 22}, 0, p.text.with_alpha(0.15f * ca));
        x += 13;
    };
    float x = bar.x + 12;
    // source: wallhaven on/off
    w::icon_button(c, ID("sl.remote"), {x, bar.y + 4, btn, btn}, "󰇚", 16, remote_).click([this] { set_remote(!remote_); });
    x += btn;
    sep(x);
    if (!remote_) {
        const char* gl[3] = {"󰸉", "󰋩", "󰕧"};  // all, still, video
        for (int i = 0; i < 3; ++i) {
            w::icon_button(c, ID("sl.kind", i), {x, bar.y + 4, btn, btn}, gl[i], 15, kind_ == i).click([this, i] { set_filter(i, hue_); });
            x += btn + 4;
        }
    } else {
        const char* gl[4] = {"󰓎", "󰥔", "󰒝", "󰈈"};  // top, latest, random, most viewed
        for (int i = 0; i < 4; ++i) {
            w::icon_button(c, ID("sl.sort", i), {x, bar.y + 4, btn, btn}, gl[i], 15, wh_sort_ == i).click([this, i] {
                if (wh_sort_ == i) return;
                wh_sort_ = i;
                wh_fetch(false);
            });
            x += btn + 4;
        }
        // mature content: off, + sketchy, + NSFW (only with an API key); a red shield when on
        x += 4;
        Rect mr{x, bar.y + 4, 34, 34};
        uint64_t mid = ID("sl.mature");
        float hv = ui_.hover(mid);
        Color red{0.92f, 0.26f, 0.26f, 1};
        bool on = wh_mature_ > 0;
        if (on) fill(cr, mr, 17, red.with_alpha((0.85f + 0.15f * hv) * ca));
        else if (hv > 0.01f) fill(cr, mr, 17, p.text.with_alpha(0.08f * hv * ca));
        glyph(cr, mr.cx(), mr.cy(), on ? "󰒘" : "󰒙", 15, (on ? Color{1, 1, 1, 1} : p.text_dim).with_alpha(ca));
        if (wh_mature_ == 2) fill(cr, {mr.right() - 9, mr.y + 2, 8, 8}, 4, Color{1, 1, 1, ca});  // NSFW too
        ui_.add(mid, mr).click([this] {
            int states = config().wallhaven_key.empty() ? 2 : 3;
            wh_mature_ = (wh_mature_ + 1) % states;
            wh_fetch(false);
        });
        x += 34 + 4;
    }
    x -= 4;
    sep(x);
    if (remote_) {
        // the search: type anywhere
        Rect sr{x, bar.y + 6, search_w, bh - 12};
        fill(cr, sr, sr.h / 2, p.text.with_alpha(0.07f * ca));
        glyph(cr, sr.x + 15, sr.cy(), "󰍉", 13, p.text_dim.with_alpha(ca));
        bool blink = std::fmod(now_mono(), 1.1) < 0.7;
        std::string shown = wh_query_.empty() ? "" : wh_query_;
        if (shown.empty()) text(cr, sr.x + 30, sr.cy() - 8, "Escribe para buscar", {ui(12), p.text_faint.with_alpha(ca)});
        else {
            Size ts = measure(shown, {ui(12.5f)});
            text(cr, sr.x + 30, sr.cy() - 8, shown, {ui(12.5f), p.text.with_alpha(ca), Align::Left, sr.w - 40});
            if (blink) fill(cr, {sr.x + 31 + std::min(ts.w, sr.w - 40), sr.cy() - 8, 1.5f, 16}, 0, p.accent.with_alpha(ca));
        }
        x += search_w;
        sep(x);
    }
    for (int i = 0; i < ncol; ++i) {
        Rect r{x, bar.cy() - dot / 2, dot, dot};
        uint64_t id = ID("sl.hue", i);
        float hv = ui_.hover(id);
        Color col = remote_ ? Color::hex(static_cast<uint32_t>(std::stoul(kWhColors[i], nullptr, 16)))
                  : i == 12 ? Color{0.62f, 0.62f, 0.66f, 1}
                            : hsv(i * 30.0f, 0.62f, 0.92f);
        Rect rr = r.inset(-hv * 2);
        fill(cr, rr, rr.w / 2, col.with_alpha(ca));
        if (hue_ == i) stroke(cr, r.inset(-4), (dot + 8) / 2, p.text.with_alpha(0.9f * ca), 2);
        ui_.add(id, r.inset(-3)).click([this, i] { set_filter(kind_, hue_ == i ? -1 : i); });
        x += dot + dgap;
    }
    cairo_restore(cr);

    // wallhaven status under the bar: loading dots, or a failure
    if (remote_ && (wh_loading_ || wh_failed_ || (visible_.empty() && !wh_loading_))) {
        float y = bar.bottom() + 22;
        if (wh_loading_) {
            for (int i = 0; i < 3; ++i) {
                float ph = static_cast<float>(std::fmod(now_mono() * 2.2 + i * 0.33, 1.0));
                float s2 = 3 + 2.5f * std::sin(ph * static_cast<float>(M_PI));
                fill(cr, {W / 2 - 18 + i * 18 - s2, y - s2, 2 * s2, 2 * s2}, s2, p.accent.with_alpha(0.9f * ca));
            }
        } else {
            text(cr, W / 2, y - 9, wh_failed_ ? "Sin conexión con wallhaven" : "Nada encontrado",
                 {ui(12.5f, "SemiBold"), p.text_dim.with_alpha(ca), Align::Center});
        }
    }
}

// wallhaven: a grid of cards, scrolling, more pages as it nears the end.
void WallPicker::grid_reveal(int index) {
    if (index < 0 || grid_row_h_ <= 0) return;
    float top = (index / grid_cols_) * grid_row_h_;
    if (top < grid_scroll_.target) grid_scroll_.set(top);
    else if (top + grid_row_h_ > grid_scroll_.target + grid_view_h_) grid_scroll_.set(top + grid_row_h_ - grid_view_h_);
}

void WallPicker::draw_wh_grid(cairo_t* cr, float W, float H, float ie) {
    const auto& p = T();
    double now = now_mono();
    grid_scroll_.update(1.0f / 60.0f);
    float gw = std::min(W - 120, 1560.0f);
    grid_cols_ = gw > 1300 ? 5 : gw > 1000 ? 4 : 3;
    float gap = 18;
    float cw = std::floor((gw - gap * (grid_cols_ - 1)) / grid_cols_), ch = std::round(cw * 9 / 16);
    grid_row_h_ = ch + gap;
    Rect area{std::round(W / 2 - gw / 2), 128, gw, H - 128 - 24};
    grid_top_ = area.y;
    grid_view_h_ = area.h;
    int n = static_cast<int>(visible_.size());
    float content = grid_content_h();
    ui_.add(ID("wh.area"), area).scroll([this](float dy) { canvas_scroll(dy); });
    bool input = !inserting_ && !leaving_;
    float t_in = static_cast<float>(now - grid_t0_);
    Color acc = p.accent;
    Rect chosen_r{};
    int chosen = inserting_ ? ins_index_ : -1;
    cairo_save(cr);
    cairo_rectangle(cr, area.x - 30, area.y - 6, area.w + 60, area.h + 12);
    cairo_clip(cr);
    ui_.push_clip(area);
    for (int i = 0; i < n; ++i) {
        int row = i / grid_cols_, col = i % grid_cols_;
        Rect r{area.x + col * (cw + gap), area.y + row * grid_row_h_ - grid_scroll_.value, cw, ch};
        if (r.bottom() < area.y - 10 || r.y > area.bottom() + 10) continue;
        const Wallpaper& wp = walls_[visible_[i]];
        // cards rise in, row by row
        int vrow = static_cast<int>((r.y - area.y) / grid_row_h_);
        float k = ease_out((t_in - 0.03f * std::max(0, vrow) - 0.01f * col) / 0.35f);
        if (k <= 0.01f) continue;
        if (i == chosen) {
            chosen_r = r;
            continue;  // drawn last, growing
        }
        uint64_t id = ID("wh.card", i);
        float hv = input ? ui_.hover(id) : 0;
        bool sel = i == sel_;
        float lift = 4 * hv + (sel ? 2 : 0);
        Rect q{r.x, r.y + (1 - k) * 26 - lift, r.w, r.h};
        float fade = k * (1.0f - ie);
        // light under the hovered / selected card
        if (hv > 0.01f || sel) {
            float g = std::max(hv, sel ? 0.7f : 0.0f);
            for (int s = 0; s < 3; ++s) {
                static const float wd[3] = {18, 10, 4}, al[3] = {0.06f, 0.12f, 0.3f};
                rrect(cr, q, 12);
                cairo_set_source_rgba(cr, acc.r, acc.g, acc.b, al[s] * g * fade);
                cairo_set_line_width(cr, wd[s]);
                cairo_stroke(cr);
            }
        }
        cairo_surface_t* img = wp.thumb.empty() ? nullptr : slice_image(wp.thumb, static_cast<int>(cw), static_cast<int>(ch), 0);
        if (img) {
            cairo_set_source_surface(cr, img, std::round(q.x), std::round(q.y));
            cairo_paint_with_alpha(cr, fade);
        } else {
            // still downloading: a soft shimmer
            float sh = 0.05f + 0.04f * std::sin(static_cast<float>(now) * 4 + i * 0.7f);
            fill(cr, q, 12, p.text.with_alpha(sh * fade));
        }
        if (wp.mature) {
            // mature: a red rim and a small badge
            Color red{0.92f, 0.26f, 0.26f, 1};
            stroke(cr, q.inset(1), 11, red.with_alpha(0.95f * fade), 2.5f);
            Rect badge{q.right() - 44, q.y + 8, 36, 20};
            fill(cr, badge, 10, red.with_alpha(0.9f * fade));
            text(cr, badge.cx(), badge.y + 2, "+18", {ui(11, "Bold"), Color{1, 1, 1, fade}, Align::Center});
        }
        if (sel) stroke(cr, q.inset(wp.mature ? -3 : 0), 12, acc.with_alpha(0.95f * fade), 2.5f);
        else if (hv > 0.01f) stroke(cr, q, 12, acc.with_alpha(0.6f * hv * fade), 1.5f);
        if ((hv > 0.01f || sel) && !wp.resolution.empty()) {
            // resolution, on a small dark chip
            Text tt{ui(11, "SemiBold"), Color{1, 1, 1, 0.95f * fade}};
            Size ts = measure(wp.resolution, tt);
            Rect chip{q.x + 10, q.bottom() - 30, ts.w + 18, 22};
            fill(cr, chip, 11, Color{0, 0, 0, 0.55f * fade * std::max(hv, sel ? 1.0f : 0.0f)});
            text(cr, chip.x + 9, chip.y + 3, wp.resolution, tt);
        }
        if (input)
            ui_.add(id, r).click([this, i] {
                if (i == sel_) slices_close(true);
                else {
                    sel_ = i;
                    request();
                }
            });
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::Ctx c{cr, ui_};
    w::scrollbar(c, {area.right() + 14, area.y + 4, 3, area.h - 8}, grid_scroll_.value, content, area.h);
    if (input && grid_scroll_.value > content - area.h - grid_row_h_ * 2) wh_fetch(true);

    // the chosen card grows into the screen
    if (chosen >= 0 && chosen < n) {
        const Wallpaper& wp = walls_[visible_[chosen]];
        Rect from = chosen_r.w > 0 ? chosen_r : Rect{W / 2 - cw / 2, H / 2 - ch / 2, cw, ch};
        Rect q{lerp(from.x, 0, ie), lerp(from.y, 0, ie), lerp(from.w, W, ie), lerp(from.h, H, ie)};
        cairo_save(cr);
        rrect(cr, q, lerp(12, 0, ie));
        cairo_clip(cr);
        if (!wp.thumb.empty() && !starts_with(wp.thumb, "http"))
            if (cairo_surface_t* th = ImageCache::get().request(wp.thumb, 520)) cover(cr, th, q, 1, CAIRO_FILTER_BILINEAR);
        cairo_restore(cr);
        if (downloading_ && ie > 0.98f) {
            // still on its way: three dots in the middle
            for (int d = 0; d < 3; ++d) {
                float ph = static_cast<float>(std::fmod(now * 2.2 + d * 0.33, 1.0));
                float s2 = 4 + 3 * std::sin(ph * static_cast<float>(M_PI));
                fill(cr, {W / 2 - 22 + d * 22 - s2, H / 2 - s2, 2 * s2, 2 * s2}, s2, Color{1, 1, 1, 0.9f});
            }
        }
    }
}

void WallPicker::draw_slices(cairo_t* cr, float W, float H, float dt) {
    const auto& p = T();
    last_w_ = W;
    last_h_ = H;
    int sc = std::max(1, scale());
    double now = now_mono();
    if (left_) return;  // back in the island: nothing left to show
    if (inserted_) {
        // fading out over the new backdrop: the picture whole
        const Wallpaper* wp = ins_index_ >= 0 && ins_index_ < static_cast<int>(visible_.size()) ? &walls_[visible_[ins_index_]] : nullptr;
        if (big_fit_) cover(cr, big_fit_, {0, 0, W, H}, 1, CAIRO_FILTER_FAST);
        else if (wp && !wp->thumb.empty())
            if (cairo_surface_t* th = ImageCache::get().request(wp->thumb, 520)) cover(cr, th, {0, 0, W, H}, 1, CAIRO_FILTER_BILINEAR);
        return;
    }
    pos_.update(dt);
    int n = static_cast<int>(visible_.size());
    float t_open = static_cast<float>(now - open_t0_);
    float t_leave = leaving_ ? static_cast<float>(now - leave_t0_) : 0.0f;
    float ins = inserting_ ? static_cast<float>((now - ins_t0_) / 0.6) : 0.0f;
    float ie = ease_io(ins);
    if (pos_.settled() && !inserting_ && !leaving_) request_big();

    // Everything comes out of the island and goes back into it.
    float ix = W / 2, iy = island_y();

    // ── veil: a circle opening from the island (closing back into it) ──
    // (inserting, it just fades: the picture takes over)
    float veil_in = ease_out(t_open / 0.45f) * (leaving_ ? 1.0f - ease_in(t_leave / 0.42f) : 1.0f);
    float veil_a = 1.0f - ie;
    if (veil_in > 0.005f && veil_a > 0.005f) {
        std::string vk = size_key(W, H, 0) + "@" + std::to_string(sc);
        if (veil_key_ != vk) {
            if (veil_) cairo_surface_destroy(veil_);
            veil_ = nullptr;
            veil_key_ = vk;
            auto out = std::make_shared<cairo_surface_t*>(nullptr);
            Loop::get().async(
                [out, W, H, sc] {
                    cairo_surface_t* v = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(W) * sc,
                                                                    static_cast<int>(H) * sc);
                    cairo_t* c = cairo_create(v);
                    cairo_scale(c, sc, sc);
                    cairo_set_source_rgba(c, 0.02, 0.02, 0.03, 0.62);
                    cairo_paint(c);
                    cairo_pattern_t* vg = cairo_pattern_create_radial(W / 2, H / 2, H * 0.3, W / 2, H / 2, W * 0.75);
                    cairo_pattern_add_color_stop_rgba(vg, 0, 0, 0, 0, 0);
                    cairo_pattern_add_color_stop_rgba(vg, 1, 0, 0, 0, 0.5);
                    cairo_set_source(c, vg);
                    cairo_paint(c);
                    cairo_pattern_destroy(vg);
                    cairo_destroy(c);
                    cairo_surface_set_device_scale(v, sc, sc);
                    *out = v;
                },
                [this, out, vk] {
                    if (veil_key_ == vk && !veil_) veil_ = *out;
                    else cairo_surface_destroy(*out);
                    request();
                });
        }
        bool full = veil_in > 0.999f;
        cairo_save(cr);
        if (!full) {
            cairo_arc(cr, ix, iy, std::hypot(W, H) * veil_in, 0, 2 * M_PI);
            cairo_clip(cr);
        }
        if (veil_) {
            cairo_set_source_surface(cr, veil_, 0, 0);
            cairo_paint_with_alpha(cr, veil_a);
        } else {
            cairo_set_source_rgba(cr, 0.02, 0.02, 0.03, 0.65 * veil_a);
            cairo_paint(cr);
        }
        cairo_restore(cr);
    }
    ui_.add(ID("sl.bg"), {0, 0, W, H});  // clicks off the slices reach canvas_button

    // ── the row (or, for wallhaven, the grid) ──
    Geo g = geo_for(W, H);
    Color glow = p.accent;
    bool input = !inserting_ && !leaving_ && t_open > 0.3f;
    if (remote_) {
        if (!leaving_) draw_wh_grid(cr, W, H, ie);
    } else if (n > 0) {
        auto size_at = [&](float d, float& w, float& h) {
            float e = ease_io(std::min(1.0f, std::fabs(d)));
            w = lerp(g.big_w, g.small_w, e);
            h = lerp(g.big_h, g.small_h, e);
        };
        int i0 = std::clamp(static_cast<int>(std::floor(pos_.value)), 0, n - 1);
        float frac = pos_.value - i0;
        int span = static_cast<int>(W / (g.small_w + g.gap) / 2) + 4;
        int lo = std::max(0, i0 - span), hi = std::min(n - 1, i0 + span + 1);
        size_t cnt = static_cast<size_t>(hi - lo + 1);
        std::vector<float> left(cnt), wid(cnt), hgt(cnt);
        for (int i = lo; i <= hi; ++i) size_at(static_cast<float>(i) - pos_.value, wid[i - lo], hgt[i - lo]);
        // slices lean alike, so they nest: a slice takes its width, plus
        // whatever lean it has beyond a small one's
        auto foot = [&](int i) { return wid[i - lo] + (g.lean(hgt[i - lo]) - g.lean(g.small_h)); };
        float f0 = foot(i0), f1 = i0 + 1 <= hi ? foot(i0 + 1) : f0;
        left[i0 - lo] = W / 2 - (f0 + g.lean(g.small_h)) / 2 - frac * (f0 / 2 + g.gap + f1 / 2);
        for (int i = i0 + 1; i <= hi; ++i) left[i - lo] = left[i - 1 - lo] + foot(i - 1) + g.gap;
        for (int i = i0 - 1; i >= lo; --i) left[i - lo] = left[i + 1 - lo] - g.gap - foot(i);

        std::vector<int> order;
        for (int i = lo; i <= hi; ++i) order.push_back(i);
        std::sort(order.begin(), order.end(),
                  [&](int a, int b) { return std::fabs(a - pos_.value) > std::fabs(b - pos_.value); });
        float big_lean = g.lean(g.big_h), small_lean = g.lean(g.small_h);
        // queue the pictures, nearest first
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const Wallpaper& wp = walls_[visible_[*it]];
            if (wp.thumb.empty()) continue;
            float a = std::fabs(*it - pos_.value);
            if (a < 1.5f) slice_image(wp.thumb, static_cast<int>(g.big_w + big_lean), static_cast<int>(g.big_h), -1);
            if (a < 0.5f) slice_image(wp.thumb, static_cast<int>(g.big_w + big_lean), static_cast<int>(g.big_h), big_lean);
            else slice_image(wp.thumb, static_cast<int>(g.small_w + small_lean), static_cast<int>(g.small_h), small_lean);
        }
        cairo_surface_t* glow_small = glow_image(g.small_w, g.small_h, small_lean, glow);
        cairo_surface_t* glow_big = glow_image(g.big_w, g.big_h, big_lean, glow);
        cairo_surface_t* dark_mask = shape_image(g.small_w, g.small_h, small_lean);
        float filt = filter_t0_ > 0 ? ease_out(static_cast<float>(now - filter_t0_) / 0.3f) : 1.0f;

        for (int i : order) {
            float w = wid[i - lo], h = hgt[i - lo], sk = g.lean(h);
            float x = left[i - lo], y = g.cy - h / 2;
            float a = std::fabs(static_cast<float>(i) - pos_.value);
            if (x + w + sk < -40 || x > W + 40) continue;
            const Wallpaper& wp = walls_[visible_[i]];
            bool chosen = inserting_ && i == ins_index_;
            // out of the island (and back in), nearest first
            float k_in = ease_out_back((t_open - 0.06f - 0.022f * std::min(a, 16.0f)) / 0.5f);
            float k_out = leaving_ ? 1.0f - ease_in((t_leave - 0.012f * std::max(0.0f, 16.0f - a)) / 0.3f) : 1.0f;
            float k = std::min(k_in, k_out);
            if (k <= 0.01f) continue;
            float fade = clamp01(k * 2) * (chosen ? 1.0f : 1.0f - ie) * filt;
            if (fade <= 0.01f) continue;
            bool flying = k < 0.999f;
            float qx = x, qy = y + (1 - filt) * 30, qw = w, qh = h, qsk = sk, rad = 12;
            if (chosen) {  // the insertion: straighten and grow to the whole screen
                qx = lerp(x, 0, ie);
                qy = lerp(y, 0, ie);
                qw = lerp(w, W, ie);
                qh = lerp(h, H, ie);
                qsk = lerp(sk, 0, ie);
                rad = lerp(12, 0, ie);
            }
            uint64_t id = ID("sl.slice", i);
            float hv = input && !chosen ? ui_.hover(id) : 0;
            float cx = qx + (qw + qsk) / 2, cy2 = qy + qh / 2;
            cairo_save(cr);
            if (flying) {
                // scaled about the island while it pours out
                float s = lerp(0.04f, 1.0f, k);
                cairo_translate(cr, lerp(ix, cx, k), lerp(iy, cy2, k));
                cairo_scale(cr, s, s);
                cairo_translate(cr, -cx, -cy2);
            } else if (hv > 0.01f && a > 0.5f) {
                float s = 1.0f + 0.06f * hv;
                cairo_translate(cr, cx, cy2);
                cairo_scale(cr, s, s);
                cairo_translate(cr, -cx, -cy2);
            }
            // backlight: strongest round the selection, fading out to the sides
            float light = chosen ? 1.0f - ie : a < 0.5f ? 1.0f : std::max(0.6f * clamp01(1.0f - (a - 1) * 0.2f), 0.8f * hv);
            light *= fade * (flying ? clamp01((k - 0.6f) / 0.4f) : 1.0f);
            if (light > 0.01f) {
                float u = clamp01((qw - g.small_w) / std::max(1.0f, g.big_w - g.small_w));
                float ext = clamp01(1.0f - std::max(0.0f, qh - g.big_h) / g.big_h);
                for (int s = 0; s < 2; ++s) {
                    cairo_surface_t* sp = s == 0 ? glow_small : glow_big;
                    float al = (s == 0 ? 1.0f - u : u) * light * ext;
                    if (!sp || al <= 0.01f) continue;
                    float ww = s == 0 ? g.small_w + small_lean : g.big_w + big_lean;
                    float hh = s == 0 ? g.small_h : g.big_h;
                    cairo_set_source_surface(cr, sp, std::round(cx - ww / 2 - kGlowPad), std::round(cy2 - hh / 2 - kGlowPad));
                    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
                    cairo_paint_with_alpha(cr, std::min(1.0f, al));
                }
            }
            // the picture: at rest a copy made at its size, already cut to shape
            bool small_rest = !chosen && std::fabs(qw - g.small_w) < 0.5f && std::fabs(qh - g.small_h) < 0.5f;
            bool big_rest = !chosen && std::fabs(qw - g.big_w) < 0.5f && std::fabs(qh - g.big_h) < 0.5f;
            float darken = !chosen && a > 0.5f ? std::min(0.5f, 0.12f + 0.04f * a) * (1.0f - hv) : 0.0f;
            const std::string& src = wp.video ? wp.thumb : wp.path;
            cairo_surface_t* cut = nullptr;
            if (!wp.thumb.empty() && (small_rest || big_rest))
                cut = slice_image(wp.thumb, static_cast<int>(qw + qsk), static_cast<int>(qh), qsk);
            if (cut) {
                float ox = std::round(qx), oy = std::round(qy);
                cairo_set_source_surface(cr, cut, ox, oy);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
                cairo_paint_with_alpha(cr, fade);
                if (darken > 0.01f && small_rest && dark_mask) {
                    cairo_set_source_rgba(cr, 0, 0, 0, darken * fade);
                    cairo_mask_surface(cr, dark_mask, ox, oy);
                }
            } else {
                cairo_save(cr);
                quad_path(cr, qx, qy, qw, qh, qsk, rad);
                cairo_clip(cr);
                Rect box{qx, qy, qw + qsk, qh};
                cairo_surface_t* bigc = wp.thumb.empty() || chosen
                                            ? nullptr
                                            : slice_image(wp.thumb, static_cast<int>(g.big_w + big_lean), static_cast<int>(g.big_h), -1);
                if (chosen && big_fit_ && big_path_ == src) {
                    cover(cr, big_fit_, box, 1, ie < 0.999f ? CAIRO_FILTER_FAST : CAIRO_FILTER_BILINEAR);
                } else if (bigc) {
                    // changing size: the big copy, centred (narrowing shows its middle)
                    cairo_set_source_surface(cr, bigc, std::round(box.x + (box.w - g.big_w - big_lean) / 2),
                                             std::round(box.y + (box.h - g.big_h) / 2));
                    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
                    cairo_paint_with_alpha(cr, fade);
                } else if (chosen && !wp.thumb.empty() && !starts_with(wp.thumb, "http")) {
                    if (cairo_surface_t* th = ImageCache::get().request(wp.thumb, 520)) cover(cr, th, box, 1, CAIRO_FILTER_FAST);
                } else {
                    set(cr, p.surface_hi.with_alpha(0.7f * fade));  // still being made
                    cairo_paint(cr);
                }
                if (darken > 0.01f) {
                    cairo_set_source_rgba(cr, 0, 0, 0, darken * fade);
                    cairo_paint(cr);
                }
                cairo_restore(cr);
            }
            // rim in the accent on the selection
            if (!chosen || ie < 0.9f) {
                quad_path(cr, qx, qy, qw, qh, qsk, rad);
                Color rim = a < 0.5f ? glow : mix(Color{1, 1, 1, 1}, glow, hv);
                cairo_set_source_rgba(cr, rim.r, rim.g, rim.b, (a < 0.5f ? 0.95f : 0.18f + 0.6f * hv) * fade * (1.0f - ie));
                cairo_set_line_width(cr, a < 0.5f ? 2.5 : 1.2);
                cairo_stroke(cr);
            }
            if (wp.video && !chosen && a < 0.5f) {  // a small play mark, no words
                float px = qx + qsk * 0.15f + qw / 2, py = qy + qh - 24;
                cairo_move_to(cr, px - 6, py - 8);
                cairo_line_to(cr, px + 8, py);
                cairo_line_to(cr, px - 6, py + 8);
                cairo_close_path(cr);
                cairo_set_source_rgba(cr, 1, 1, 1, 0.9 * fade);
                cairo_fill(cr);
            }
            cairo_restore(cr);
            if (input) {
                ui_.add(id, {x, y, w + sk, h}).click([this, i] {
                    if (i == sel_) slices_close(true);
                    else slices_move(i - sel_);
                });
            }
        }
    }

    // the filters, out of the island too
    float bar_in = leaving_ ? 1.0f - clamp01(t_leave / 0.25f) : clamp01((t_open - 0.1f) / 0.45f);
    if (!inserting_) draw_filter_bar(cr, W, H, bar_in);

    // done: it is the screen now (and, once drawn, the backdrop under it)
    if (inserting_ && ins >= 1.0f && !downloading_ && (backdrop_ready_ || now - ins_t0_ > (remote_ ? 8.0 : 2.5))) {
        inserting_ = false;
        inserted_ = true;
        close();
    }
    if (leaving_ && t_leave > 0.5f) {
        leaving_ = false;
        left_ = true;
        close();
    }
}

}  // namespace island
