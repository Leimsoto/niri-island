#include "services/images.hpp"

#include <iostream>

#include <curl/curl.h>
#include <dirent.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <mutex>
#include <sys/stat.h>
#include <vector>

#include "core/loop.hpp"

namespace island {

// ── http ───────────────────────────────────────────────────────────────────

static size_t curl_write(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return out->size() > (32u << 20) ? 0 : size * nmemb;
}

// One DNS cache, TLS session cache and connection pool for every request:
// only the first lookup of a host pays for a slow resolver.
static std::mutex g_share_mu[CURL_LOCK_DATA_LAST];
static void share_lock(CURL*, curl_lock_data d, curl_lock_access, void*) { g_share_mu[d].lock(); }
static void share_unlock(CURL*, curl_lock_data d, void*) { g_share_mu[d].unlock(); }
static CURLSH* share() {
    static CURLSH* sh = [] {
        CURLSH* s = curl_share_init();
        curl_share_setopt(s, CURLSHOPT_LOCKFUNC, share_lock);
        curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, share_unlock);
        curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
        curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        return s;
    }();
    return sh;
}

bool http_get(const std::string& url, std::string& body, long timeout_s) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) return false;
    curl_easy_setopt(c, CURLOPT_SHARE, share());
    curl_easy_setopt(c, CURLOPT_DNS_CACHE_TIMEOUT, 600L);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "niri-island/2.0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return rc == CURLE_OK && code >= 200 && code < 300;
}

bool http_download(const std::string& url, const std::string& dest, long timeout_s) {
    std::string body;
    if (!http_get(url, body, timeout_s) || body.empty()) return false;
    return write_file_atomic(dest, body);
}

// ── decode ─────────────────────────────────────────────────────────────────

static cairo_surface_t* surface_from_pixbuf(GdkPixbuf* pb) {
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    cairo_surface_flush(s);
    unsigned char* dst = cairo_image_surface_get_data(s);
    int dstride = cairo_image_surface_get_stride(s);
    const guchar* src = gdk_pixbuf_read_pixels(pb);
    int sstride = gdk_pixbuf_get_rowstride(pb);
    int ch = gdk_pixbuf_get_n_channels(pb);
    bool alpha = gdk_pixbuf_get_has_alpha(pb);
    for (int y = 0; y < h; ++y) {
        const guchar* sp = src + y * sstride;
        uint32_t* dp = reinterpret_cast<uint32_t*>(dst + y * dstride);
        for (int x = 0; x < w; ++x) {
            uint32_t r = sp[0], g = sp[1], b = sp[2], a = alpha ? sp[3] : 255;
            sp += ch;
            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            *dp++ = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

static std::string resolve_source(const std::string& src) {
    if (starts_with(src, "file://")) {
        char* local = g_filename_from_uri(src.c_str(), nullptr, nullptr);
        if (local) {
            std::string r = local;
            g_free(local);
            return r;
        }
        return src.substr(7);
    }
    if (starts_with(src, "http://") || starts_with(src, "https://")) {
        std::string dir = cache_dir() + "/art";
        make_dirs(dir);
        char name[32];
        snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash_str(src)));
        std::string dest = dir + "/" + name;
        struct stat st {};
        if (stat(dest.c_str(), &st) == 0 && st.st_size > 0) return dest;
        return http_download(src, dest) ? dest : "";
    }
    return src;
}

cairo_surface_t* load_image_blocking(const std::string& src, int max_px) {
    std::string path = resolve_source(src);
    if (path.empty() || !file_exists(path)) return nullptr;
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file_at_scale(path.c_str(), max_px, max_px, TRUE, &err);
    if (!pb) {
        if (err) g_error_free(err);
        return nullptr;
    }
    GdkPixbuf* oriented = gdk_pixbuf_apply_embedded_orientation(pb);
    cairo_surface_t* s = surface_from_pixbuf(oriented ? oriented : pb);
    if (oriented) g_object_unref(oriented);
    g_object_unref(pb);
    return s;
}

cairo_surface_t* load_image_full(const std::string& path) {
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (!pb) {
        if (err) g_error_free(err);
        return nullptr;
    }
    cairo_surface_t* s = surface_from_pixbuf(pb);
    g_object_unref(pb);
    return s;
}

cairo_surface_t* surface_from_rgba(int w, int h, int stride, bool alpha, int channels, const uint8_t* data,
                                   size_t len, int max_px) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || channels < 3 || channels > 4) return nullptr;
    if (static_cast<size_t>(stride) * (h - 1) + static_cast<size_t>(w) * channels > len) return nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, alpha, 8, w, h, stride, nullptr, nullptr);
    if (!pb) return nullptr;
    GdkPixbuf* scaled = pb;
    if (w > max_px || h > max_px) {
        float s = std::min(max_px / float(w), max_px / float(h));
        scaled = gdk_pixbuf_scale_simple(pb, std::max(1, int(w * s)), std::max(1, int(h * s)), GDK_INTERP_BILINEAR);
    }
    cairo_surface_t* out = surface_from_pixbuf(scaled);
    if (scaled != pb) g_object_unref(scaled);
    g_object_unref(pb);
    return out;
}

Color dominant_color(cairo_surface_t* s) {
    if (!s) return {};
    cairo_surface_flush(s);
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    const unsigned char* d = cairo_image_surface_get_data(s);
    double r = 0, g = 0, b = 0, wsum = 0;
    int step = std::max(1, std::min(w, h) / 24);
    for (int y = 0; y < h; y += step) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(d + y * stride);
        for (int x = 0; x < w; x += step) {
            uint32_t p = row[x];
            double a = ((p >> 24) & 0xff) / 255.0;
            if (a < 0.5) continue;
            double pr = ((p >> 16) & 0xff) / 255.0 / a, pg = ((p >> 8) & 0xff) / 255.0 / a, pb = (p & 0xff) / 255.0 / a;
            double mx = std::max({pr, pg, pb}), mn = std::min({pr, pg, pb});
            double sat = mx - mn;
            // Vivid, mid-bright pixels speak for the image; greys and blacks do not.
            double wgt = 0.05 + sat * sat * 4.0 * (1.0 - std::abs(mx - 0.6));
            r += pr * wgt;
            g += pg * wgt;
            b += pb * wgt;
            wsum += wgt;
        }
    }
    if (wsum <= 0) return Color{0.8f, 0.75f, 0.95f, 1};
    Color c{float(r / wsum), float(g / wsum), float(b / wsum), 1};
    // Lift into a readable accent on a dark island.
    float mx = std::max({c.r, c.g, c.b});
    if (mx > 0.01f) {
        float k = std::min(1.0f / mx, 1.6f) * 0.95f;
        c = {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k), 1};
    }
    if (c.luminance() < 0.45f) c = mix(c, Color{1, 1, 1, 1}, 0.3f);
    return c;
}

// ── icon theme ─────────────────────────────────────────────────────────────

namespace {

struct IconIndex {
    std::mutex mu;
    bool built = false;
    // name -> (size score, path); scalable icons score as 512.
    std::unordered_map<std::string, std::vector<std::pair<int, std::string>>> icons;
};

IconIndex& icon_index() {
    static IconIndex idx;
    return idx;
}

std::string gtk_icon_theme() {
    std::string text;
    if (read_file(home_dir() + "/.config/gtk-3.0/settings.ini", text)) {
        for (auto& line : split(text, '\n')) {
            auto t = trim(line);
            if (starts_with(t, "gtk-icon-theme-name")) {
                auto eq = t.find('=');
                if (eq != std::string::npos) return trim(t.substr(eq + 1));
            }
        }
    }
    return "hicolor";
}

std::vector<std::string> theme_inherits(const std::string& dir) {
    std::string text;
    std::vector<std::string> out;
    if (!read_file(dir + "/index.theme", text)) return out;
    for (auto& line : split(text, '\n')) {
        auto t = trim(line);
        if (starts_with(t, "Inherits=")) {
            for (auto& p : split(t.substr(9), ',')) out.push_back(trim(p));
            break;
        }
    }
    return out;
}

int size_from_path(const std::string& path) {
    if (path.find("scalable") != std::string::npos) return 512;
    // .../48x48/... or .../48/...
    for (auto& part : split(path, '/')) {
        int a = 0, b = 0;
        if (sscanf(part.c_str(), "%dx%d", &a, &b) == 2 && a > 0) return a;
        if (sscanf(part.c_str(), "%d@", &a) == 1 && a > 0) return a;
        bool digits = !part.empty();
        for (char c : part) digits &= std::isdigit(static_cast<unsigned char>(c)) != 0;
        if (digits) return std::atoi(part.c_str());
    }
    return 32;
}

void scan(const std::string& dir, int depth, IconIndex& idx, int priority) {
    if (depth > 4) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            struct stat st {};
            if (stat(full.c_str(), &st) == 0) is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            scan(full, depth + 1, idx, priority);
            continue;
        }
        size_t dot = n.rfind('.');
        if (dot == std::string::npos) continue;
        std::string ext = n.substr(dot + 1);
        if (ext != "png" && ext != "svg" && ext != "xpm") continue;
        std::string name = n.substr(0, dot);
        auto& v = idx.icons[name];
        // Themes earlier in the inheritance chain win: encode as priority band.
        v.emplace_back(priority * 10000 + size_from_path(full), full);
    }
    closedir(d);
}

void build_index(IconIndex& idx) {
    std::vector<std::string> bases = {home_dir() + "/.local/share/icons", home_dir() + "/.icons", "/usr/share/icons",
                                      "/usr/local/share/icons"};
    std::vector<std::string> order;
    std::unordered_set<std::string> seen;
    std::vector<std::string> queue = {gtk_icon_theme()};
    while (!queue.empty() && order.size() < 8) {
        std::string t = queue.front();
        queue.erase(queue.begin());
        if (seen.count(t)) continue;
        seen.insert(t);
        order.push_back(t);
        for (auto& b : bases) {
            if (file_exists(b + "/" + t + "/index.theme")) {
                for (auto& i : theme_inherits(b + "/" + t)) queue.push_back(i);
                break;
            }
        }
    }
    if (!seen.count("hicolor")) order.push_back("hicolor");
    int prio = static_cast<int>(order.size()) + 1;
    for (auto& t : order) {
        for (auto& b : bases) scan(b + "/" + t, 0, idx, prio);
        prio--;
    }
    scan("/usr/share/pixmaps", 3, idx, 0);
    idx.built = true;
}

}  // namespace

std::string find_icon_blocking(const std::string& name, int size) {
    if (name.empty()) return "";
    if (name[0] == '/') return file_exists(name) ? name : "";
    if (starts_with(name, "file://")) return name;
    IconIndex& idx = icon_index();
    std::lock_guard<std::mutex> lock(idx.mu);
    if (!idx.built) build_index(idx);
    auto try_name = [&](const std::string& n) -> std::string {
        auto it = idx.icons.find(n);
        if (it == idx.icons.end()) return "";
        // Highest priority band, then the size closest above `size`.
        int best_band = -1;
        for (auto& [score, p] : it->second) best_band = std::max(best_band, score / 10000);
        std::string best;
        int best_cost = 1 << 30;
        for (auto& [score, p] : it->second) {
            if (score / 10000 != best_band) continue;
            int s = score % 10000;
            int cost = s >= size ? s - size : (size - s) * 4;
            if (s == 512) cost = 64;  // scalable: good, not perfect
            if (cost < best_cost) {
                best_cost = cost;
                best = p;
            }
        }
        return best;
    };
    std::string r = try_name(name);
    if (r.empty()) r = try_name(lower(name));
    if (r.empty()) {
        // "org.gnome.Nautilus" -> "nautilus", "Brave-browser" -> "brave-browser"
        size_t dot = name.rfind('.');
        if (dot != std::string::npos) r = try_name(lower(name.substr(dot + 1)));
    }
    return r;
}

// ── cache ──────────────────────────────────────────────────────────────────

ImageCache& ImageCache::get() {
    static ImageCache c;
    return c;
}

ImageCache::~ImageCache() { clear(); }

void ImageCache::clear() {
    for (auto& [k, e] : entries_)
        if (e.surface) cairo_surface_destroy(e.surface);
    entries_.clear();
}

cairo_surface_t* ImageCache::request(const std::string& src, int max_px) {
    if (src.empty()) return nullptr;
    std::string key = src + "@" + std::to_string(max_px);
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        it->second.used = now_mono();
        return it->second.surface;
    }
    if (loading_.count(key)) return nullptr;
    loading_.insert(key);
    struct Out {
        cairo_surface_t* s = nullptr;
        Color c;
    };
    auto out = std::make_shared<Out>();
    Loop::get().async(
        [src, max_px, out] {
            std::string path = src;
            if (starts_with(src, "icon:")) path = find_icon_blocking(src.substr(5), max_px);
            if (!path.empty()) out->s = load_image_blocking(path, max_px);
            if (out->s) out->c = dominant_color(out->s);
        },
        [this, key, out] {
            loading_.erase(key);
            Entry e;
            e.surface = out->s;
            e.color = out->c;
            e.failed = out->s == nullptr;
            e.used = now_mono();
            entries_[key] = e;
            trim();
            if (on_ready) on_ready();
        });
    return nullptr;
}

Color ImageCache::color_of(const std::string& src, int max_px, Color fallback) {
    std::string key = src + "@" + std::to_string(max_px);
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.surface) return fallback;
    return it->second.color;
}

void ImageCache::trim() {
    if (entries_.size() < 160) return;
    std::vector<std::pair<double, std::string>> byage;
    for (auto& [k, e] : entries_) byage.emplace_back(e.used, k);
    std::sort(byage.begin(), byage.end());
    for (size_t i = 0; i < byage.size() / 3; ++i) {
        auto it = entries_.find(byage[i].second);
        if (it->second.surface) cairo_surface_destroy(it->second.surface);
        entries_.erase(it);
    }
}

}  // namespace island
