#include "services/apps.hpp"

#include <cmath>
#include <dirent.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"

namespace island {

using json = nlohmann::json;

// ── desktop entries ────────────────────────────────────────────────────────

static std::string localized(const std::map<std::string, std::string>& kv, const std::string& key) {
    const char* lang = getenv("LANG");
    std::string l = lang ? lang : "";
    std::string full = l.substr(0, l.find('.'));      // es_CO
    std::string shrt = full.substr(0, full.find('_'));  // es
    for (auto& cand : {key + "[" + full + "]", key + "[" + shrt + "]", key}) {
        auto it = kv.find(cand);
        if (it != kv.end() && !it->second.empty()) return it->second;
    }
    return "";
}

static bool parse_desktop(const std::string& path, const std::string& id, AppEntry& out) {
    std::string text;
    if (!read_file(path, text)) return false;
    std::map<std::string, std::string> kv;
    bool in_main = false;
    for (auto& raw : split(text, '\n')) {
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            in_main = line == "[Desktop Entry]";
            continue;
        }
        if (!in_main) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    if (kv["Type"] != "Application") return false;
    if (kv["NoDisplay"] == "true" || kv["Hidden"] == "true") return false;
    std::string desktops = kv["OnlyShowIn"];
    if (!desktops.empty() && desktops.find("niri") == std::string::npos) return false;
    out.id = id;
    out.name = localized(kv, "Name");
    out.generic = localized(kv, "GenericName");
    out.comment = localized(kv, "Comment");
    out.keywords = localized(kv, "Keywords");
    out.icon = kv["Icon"];
    out.terminal = kv["Terminal"] == "true";
    std::string exec = kv["Exec"];
    // Drop field codes (%f %U …), keep a literal %%.
    std::string clean;
    for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            if (exec[i + 1] == '%') clean += '%';
            i++;
            continue;
        }
        clean += exec[i];
    }
    out.exec = trim(clean);
    return !out.name.empty() && !out.exec.empty();
}

static void scan_dir(const std::string& dir, const std::string& prefix, std::vector<AppEntry>& out,
                     std::unordered_set<std::string>& seen) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        if (e->d_type == DT_DIR) {
            scan_dir(full, prefix + n + "-", out, seen);
            continue;
        }
        if (!ends_with(n, ".desktop")) continue;
        std::string id = prefix + n.substr(0, n.size() - 8);
        if (seen.count(id)) continue;  // earlier dirs (user) override later ones
        seen.insert(id);
        AppEntry a;
        if (parse_desktop(full, id, a)) out.push_back(std::move(a));
    }
    closedir(d);
}

void Apps::init() {
    load_usage();
    auto list = std::make_shared<std::vector<AppEntry>>();
    Loop::get().async(
        [list] {
            std::vector<std::string> dirs;
            const char* dh = getenv("XDG_DATA_HOME");
            dirs.push_back((dh && *dh ? std::string(dh) : home_dir() + "/.local/share") + "/applications");
            const char* dd = getenv("XDG_DATA_DIRS");
            for (auto& d : split(dd && *dd ? dd : "/usr/local/share:/usr/share", ':'))
                if (!d.empty()) dirs.push_back(d + "/applications");
            dirs.push_back("/var/lib/flatpak/exports/share/applications");
            dirs.push_back(home_dir() + "/.local/share/flatpak/exports/share/applications");
            std::unordered_set<std::string> seen;
            for (auto& d : dirs) scan_dir(d, "", *list, seen);
            std::sort(list->begin(), list->end(),
                      [](const AppEntry& a, const AppEntry& b) { return lower(a.name) < lower(b.name); });
        },
        [this, list] {
            apps_ = std::move(*list);
            ready_ = true;
            if (on_change) on_change();
        });
}

int Apps::uses(const std::string& id) const {
    auto it = usage_.find(id);
    return it == usage_.end() ? 0 : it->second;
}

// Subsequence match with bonuses for word starts and contiguous runs.
static int fuzzy(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 1;
    std::string h = lower(hay);
    size_t pos = h.find(needle);
    if (pos != std::string::npos) {
        int s = 1000 - static_cast<int>(pos) * 10;
        if (pos == 0) s += 500;
        else if (!std::isalnum(static_cast<unsigned char>(h[pos - 1]))) s += 300;
        return s;
    }
    int score = 0;
    size_t hi = 0;
    int run = 0;
    for (char c : needle) {
        bool found = false;
        while (hi < h.size()) {
            if (h[hi] == c) {
                bool word_start = hi == 0 || !std::isalnum(static_cast<unsigned char>(h[hi - 1]));
                score += 10 + (word_start ? 25 : 0) + run * 8;
                run++;
                hi++;
                found = true;
                break;
            }
            run = 0;
            hi++;
        }
        if (!found) return 0;
    }
    return score;
}

std::vector<Apps::Match> Apps::search(const std::string& query, size_t limit) const {
    std::string q = lower(trim(query));
    std::vector<Match> out;
    for (auto& a : apps_) {
        int s;
        if (q.empty()) {
            s = 1 + uses(a.id) * 50;
        } else {
            s = std::max({fuzzy(a.name, q) * 3, fuzzy(a.generic, q), fuzzy(a.keywords, q), fuzzy(a.id, q),
                          fuzzy(a.exec.substr(0, a.exec.find(' ')), q)});
            if (s <= 0) continue;
            s += std::min(uses(a.id), 40) * 12;
        }
        out.push_back({&a, s});
    }
    std::stable_sort(out.begin(), out.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

void Apps::launch(const AppEntry& app) {
    usage_[app.id]++;
    save_usage();
    std::string cmd = app.exec;
    if (app.terminal) cmd = config().terminal + " -e " + cmd;
    proc::spawn_shell(cmd);
}

void Apps::load_usage() {
    std::string text;
    if (!read_file(state_dir() + "/launches.json", text)) return;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    for (auto& [k, v] : j.items())
        if (v.is_number_integer()) usage_[k] = v.get<int>();
}

void Apps::save_usage() {
    json j = json::object();
    for (auto& [k, v] : usage_) j[k] = v;
    make_dirs(state_dir());
    write_file_atomic(state_dir() + "/launches.json", j.dump());
}

// ── calculator ─────────────────────────────────────────────────────────────

namespace {
struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& str) : s(str) {}

    void ws() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    }
    bool eat(char c) {
        ws();
        if (i < s.size() && s[i] == c) {
            i++;
            return true;
        }
        return false;
    }
    double expr() {
        double v = term();
        for (;;) {
            if (eat('+')) v += term();
            else if (eat('-')) v -= term();
            else return v;
        }
    }
    double term() {
        double v = unary();
        for (;;) {
            ws();
            if (eat('*') || eat('x')) v *= unary();
            else if (eat('/')) {
                double d = unary();
                if (d == 0) ok = false;
                v = d == 0 ? 0 : v / d;
            } else if (eat('%')) {
                ws();
                // "15% of 80" / "15%80" vs modulo: treat trailing % as percent.
                if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '(' || s[i] == '.'))
                    v = std::fmod(v, unary());
                else v /= 100.0;
            } else return v;
        }
    }
    // Unary minus binds looser than ^, so -3^2 is -9.
    double unary() {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        return power();
    }
    double power() {
        double b = atom();
        if (eat('^')) return std::pow(b, unary());
        return b;
    }
    double atom() {
        ws();
        if (eat('(')) {
            double v = expr();
            if (!eat(')')) ok = false;
            return v;
        }
        if (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) {
            size_t start = i;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == '_')) i++;
            std::string num = replace_all(s.substr(start, i - start), "_", "");
            return std::atof(num.c_str());
        }
        if (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
            size_t start = i;
            while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) i++;
            std::string fn = lower(s.substr(start, i - start));
            if (fn == "pi") return M_PI;
            if (fn == "e") return M_E;
            if (fn == "tau") return 2 * M_PI;
            double a = 0;
            if (eat('(')) {
                a = expr();
                if (!eat(')')) ok = false;
            } else {
                a = unary();
            }
            if (fn == "sqrt") return std::sqrt(a);
            if (fn == "sin") return std::sin(a);
            if (fn == "cos") return std::cos(a);
            if (fn == "tan") return std::tan(a);
            if (fn == "asin") return std::asin(a);
            if (fn == "acos") return std::acos(a);
            if (fn == "atan") return std::atan(a);
            if (fn == "ln") return std::log(a);
            if (fn == "log") return std::log10(a);
            if (fn == "abs") return std::fabs(a);
            if (fn == "round") return std::round(a);
            if (fn == "floor") return std::floor(a);
            if (fn == "ceil") return std::ceil(a);
            if (fn == "exp") return std::exp(a);
            ok = false;
            return 0;
        }
        ok = false;
        return 0;
    }
};
}  // namespace

bool calc_eval(const std::string& input, double& out) {
    std::string e = replace_all(lower(trim(input)), ",", ".");
    e = replace_all(e, " of ", "*");
    e = replace_all(e, " de ", "*");
    e = replace_all(e, "×", "*");
    e = replace_all(e, "÷", "/");
    if (e.empty()) return false;
    Parser p(e);
    double v = p.expr();
    p.ws();
    if (!p.ok || p.i != e.size() || std::isnan(v) || std::isinf(v)) return false;
    out = v;
    return true;
}

std::string calc_format(double v) {
    char buf[64];
    if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) snprintf(buf, sizeof(buf), "%.0f", v);
    else snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

}  // namespace island
