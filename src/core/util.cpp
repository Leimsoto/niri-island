#include "core/util.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace island {

bool Color::parse(const std::string& s, Color& out) {
    std::string t = trim(s);
    if (t.empty() || t[0] != '#') return false;
    t = t.substr(1);
    if (t.size() != 6 && t.size() != 8) return false;
    char* end = nullptr;
    unsigned long v = std::strtoul(t.c_str(), &end, 16);
    if (!end || *end) return false;
    if (t.size() == 6) {
        out = Color::hex(static_cast<uint32_t>(v));
    } else {
        out = Color::hex(static_cast<uint32_t>(v >> 8), (v & 0xff) / 255.0f);
    }
    return true;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return std::string(s.substr(a, b - a));
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

bool starts_with(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string escape_markup(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;"; break;
            case '>': r += "&gt;"; break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&#39;"; break;
            default: r += c;
        }
    }
    return r;
}

static bool is_entity_at(std::string_view s, size_t i) {
    // &name; or &#123; or &#x1F;
    size_t j = i + 1;
    if (j >= s.size()) return false;
    if (s[j] == '#') {
        j++;
        if (j < s.size() && (s[j] == 'x' || s[j] == 'X')) j++;
        size_t digits = 0;
        while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) j++, digits++;
        return digits > 0 && j < s.size() && s[j] == ';';
    }
    size_t letters = 0;
    while (j < s.size() && std::isalnum(static_cast<unsigned char>(s[j]))) j++, letters++;
    return letters > 0 && letters < 10 && j < s.size() && s[j] == ';';
}

std::string sanitize_markup(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 16);
    std::vector<std::string> open;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '&') {
            out += is_entity_at(s, i) ? "&" : "&amp;";
            i++;
            continue;
        }
        if (c == '<') {
            size_t close = s.find('>', i);
            if (close == std::string_view::npos) {
                out += "&lt;";
                i++;
                continue;
            }
            std::string tag = lower(s.substr(i + 1, close - i - 1));
            bool closing = !tag.empty() && tag[0] == '/';
            std::string name = trim(closing ? tag.substr(1) : tag);
            size_t sp = name.find_first_of(" \t/");
            if (sp != std::string::npos) name = name.substr(0, sp);
            if (name == "br") {
                out += "\n";
            } else if (name == "b" || name == "i" || name == "u") {
                if (closing) {
                    if (!open.empty() && open.back() == name) {
                        out += "</" + name + ">";
                        open.pop_back();
                    }
                } else {
                    out += "<" + name + ">";
                    open.push_back(name);
                }
            }
            // <a>, <img> and anything else: drop the tag, keep the text around it.
            i = close + 1;
            continue;
        }
        if (c == '>') {
            out += "&gt;";
            i++;
            continue;
        }
        out += c;
        i++;
    }
    while (!open.empty()) {
        out += "</" + open.back() + ">";
        open.pop_back();
    }
    return out;
}

std::string strip_markup(std::string_view s) {
    std::string out;
    bool in_tag = false;
    for (char c : s) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out += c;
    }
    out = replace_all(out, "&amp;", "&");
    out = replace_all(out, "&lt;", "<");
    out = replace_all(out, "&gt;", ">");
    out = replace_all(out, "&quot;", "\"");
    out = replace_all(out, "&#39;", "'");
    return out;
}

size_t utf8_prev(const std::string& s, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) pos--;
    return pos;
}

size_t utf8_next(const std::string& s, size_t pos) {
    if (pos >= s.size()) return s.size();
    pos++;
    while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) pos++;
    return pos;
}

size_t utf8_length(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

double now_mono() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

int64_t now_epoch() { return static_cast<int64_t>(time(nullptr)); }

struct tm local_tm(time_t t) {
    if (t == 0) t = time(nullptr);
    struct tm out {};
    localtime_r(&t, &out);
    return out;
}

std::string fmt_time(const char* fmt, time_t t) {
    struct tm tm = local_tm(t);
    char buf[128];
    size_t n = strftime(buf, sizeof(buf), fmt, &tm);
    return std::string(buf, n);
}

std::string fmt_duration(int64_t s) {
    if (s < 0) s = 0;
    char buf[32];
    if (s >= 3600)
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)(s / 3600), (long long)((s % 3600) / 60),
                 (long long)(s % 60));
    else
        snprintf(buf, sizeof(buf), "%lld:%02lld", (long long)(s / 60), (long long)(s % 60));
    return buf;
}

std::string date_iso(int y, int m0, int d) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y % 10000, (m0 % 12) + 1, d % 100);
    return buf;
}

int days_in_month(int year, int m0) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    m0 = ((m0 % 12) + 12) % 12;
    if (m0 == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[m0];
}

int first_weekday_monday(int year, int m0) {
    struct tm t {};
    t.tm_year = year - 1900;
    t.tm_mon = m0;
    t.tm_mday = 1;
    t.tm_hour = 12;
    mktime(&t);
    return (t.tm_wday + 6) % 7;
}

std::string time_ago(int64_t epoch) {
    int64_t d = std::max<int64_t>(0, now_epoch() - epoch);
    if (d < 60) return "ahora";
    if (d < 3600) return std::to_string(d / 60) + " min";
    if (d < 86400) return std::to_string(d / 3600) + " h";
    if (d < 604800) return std::to_string(d / 86400) + " d";
    return std::to_string(d / 604800) + " sem";
}

std::string home_dir() {
    const char* h = getenv("HOME");
    return h ? h : "/tmp";
}

static std::string xdg(const char* var, const char* fallback) {
    const char* v = getenv(var);
    if (v && *v) return v;
    return home_dir() + fallback;
}

std::string config_dir() { return xdg("XDG_CONFIG_HOME", "/.config") + "/niri-island"; }
std::string state_dir() { return xdg("XDG_STATE_HOME", "/.local/state") + "/niri-island"; }
std::string cache_dir() { return xdg("XDG_CACHE_HOME", "/.cache") + "/niri-island"; }
std::string runtime_dir() {
    const char* v = getenv("XDG_RUNTIME_DIR");
    if (v && *v) return v;
    return "/tmp";
}

std::string user_dir(const char* key, const char* fallback) {
    std::string text;
    std::string want = std::string("XDG_") + key + "_DIR=";
    if (read_file(xdg("XDG_CONFIG_HOME", "/.config") + "/user-dirs.dirs", text)) {
        for (auto& line : split(text, '\n')) {
            std::string t = trim(line);
            if (!starts_with(t, want)) continue;
            std::string v = t.substr(want.size());
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            v = replace_all(v, "$HOME", home_dir());
            if (!v.empty()) return v;
        }
    }
    return home_dir() + "/" + fallback;
}

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool file_exists(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0;
}

void make_dirs(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
}

}  // namespace island
