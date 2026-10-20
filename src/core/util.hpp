#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace island {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;

    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    Rect intersect(const Rect& o) const {
        float nx = std::max(x, o.x), ny = std::max(y, o.y);
        float nr = std::min(right(), o.right()), nb = std::min(bottom(), o.bottom());
        return {nx, ny, std::max(0.0f, nr - nx), std::max(0.0f, nb - ny)};
    }
    Rect unite(const Rect& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        float nx = std::min(x, o.x), ny = std::min(y, o.y);
        return {nx, ny, std::max(right(), o.right()) - nx, std::max(bottom(), o.bottom()) - ny};
    }
};

struct Color {
    float r = 0, g = 0, b = 0, a = 1;

    Color with_alpha(float na) const { return {r, g, b, na}; }
    Color fade(float k) const { return {r, g, b, a * k}; }
    static Color hex(uint32_t rgb, float a = 1.0f) {
        return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a};
    }
    static bool parse(const std::string& s, Color& out);
    float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
};

inline Color mix(const Color& a, const Color& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
inline float smoothstep(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

// FNV-1a, used to build stable widget ids from names and indices.
inline uint64_t hash_str(std::string_view s, uint64_t seed = 1469598103934665603ull) {
    uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}
inline uint64_t ID(std::string_view name, int64_t idx = 0) {
    uint64_t h = hash_str(name);
    h ^= static_cast<uint64_t>(idx) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// ── strings ────────────────────────────────────────────────────────────────

std::string trim(std::string_view s);
std::string lower(std::string_view s);
std::vector<std::string> split(std::string_view s, char sep);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
std::string escape_markup(std::string_view s);
// Keeps the subset of Pango markup notification bodies may use (<b>, <i>, <u>)
// and escapes everything else, so a stray '&' or '<a href>' never breaks layout.
std::string sanitize_markup(std::string_view s);
std::string strip_markup(std::string_view s);

// ── utf8 ───────────────────────────────────────────────────────────────────

size_t utf8_prev(const std::string& s, size_t pos);
size_t utf8_next(const std::string& s, size_t pos);
size_t utf8_length(std::string_view s);

// ── time ───────────────────────────────────────────────────────────────────

double now_mono();     // seconds, monotonic
int64_t now_epoch();   // seconds since epoch
struct tm local_tm(time_t t = 0);
std::string fmt_time(const char* fmt, time_t t = 0);
std::string fmt_duration(int64_t seconds);   // m:ss or h:mm:ss
std::string date_iso(int y, int m0, int d);   // m0 is 0-based
int days_in_month(int year, int m0);
int first_weekday_monday(int year, int m0);   // 0 = Monday
std::string time_ago(int64_t epoch);

// ── files ──────────────────────────────────────────────────────────────────

std::string home_dir();
std::string config_dir();   // ~/.config/niri-island
std::string state_dir();    // ~/.local/state/niri-island
std::string cache_dir();    // ~/.cache/niri-island
std::string runtime_dir();
// XDG user directory ("PICTURES", "VIDEOS") from user-dirs.dirs, with a fallback.
std::string user_dir(const char* key, const char* fallback);
// Single-quotes a string for sh -c.
std::string shell_quote(const std::string& s);
bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, const std::string& data);
bool file_exists(const std::string& path);
void make_dirs(const std::string& path);

}  // namespace island
