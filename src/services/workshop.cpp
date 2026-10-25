#include "services/workshop.hpp"

#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <ftw.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include "core/util.hpp"

namespace island::workshop {

std::vector<std::string> steam_libraries() {
    std::set<std::string> found;
    for (std::string root : {home_dir() + "/.local/share/Steam", home_dir() + "/.steam/steam",
                             home_dir() + "/.var/app/com.valvesoftware.Steam/data/Steam"}) {
        std::string vdf;
        if (!read_file(root + "/steamapps/libraryfolders.vdf", vdf)) continue;
        // lines like:   "path"   "/home/me/.local/share/Steam"
        for (auto& line : split(vdf, '\n')) {
            std::string t = trim(line);
            if (!starts_with(t, "\"path\"")) continue;
            size_t a = t.find('"', 6), b = a == std::string::npos ? a : t.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string p = t.substr(a + 1, b - a - 1);
            char real[4096];
            if (realpath(p.c_str(), real)) found.insert(real);
        }
    }
    return {found.begin(), found.end()};
}

std::vector<std::string> workshop_dirs() {
    std::vector<std::string> out;
    for (auto& lib : steam_libraries()) {
        std::string d = lib + "/steamapps/workshop/content/431960";
        if (file_exists(d)) out.push_back(d);
    }
    return out;
}

bool read_item(const std::string& dir, Item& out) {
    std::string text;
    if (!read_file(dir + "/project.json", text)) return false;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    auto str = [&](const char* k) {
        auto it = j.find(k);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    out.dir = dir;
    out.title = trim(str("title"));
    out.type = lower(str("type"));
    std::string pv = str("preview");
    out.preview = pv.empty() || pv.find("..") != std::string::npos ? "" : dir + "/" + pv;
    if (!out.preview.empty() && !file_exists(out.preview)) out.preview.clear();
    if (out.title.empty()) out.title = dir.substr(dir.rfind('/') + 1);
    return true;
}

bool is_item(const std::string& path) {
    return !path.empty() && file_exists(path + "/project.json");
}

namespace {

// Workshop items the Steam accounts here are subscribed to (and have not
// disabled), from userdata/<account>/ugc/431960_subscriptions.vdf, which
// Steam rewrites as you subscribe: unsubscribing leaves the files behind for
// a while. `since` gets the newest rewrite. False when there is no such file.
bool subscriptions(const std::string& steam_root, std::set<std::string>& out, time_t& since) {
    bool any = false;
    DIR* d = opendir((steam_root + "/userdata").c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string path = steam_root + "/userdata/" + e->d_name + "/ugc/431960_subscriptions.vdf", vdf;
        if (!read_file(path, vdf)) continue;
        any = true;
        struct stat st {};
        if (stat(path.c_str(), &st) == 0) since = std::max(since, st.st_mtime);
        // blocks of  "publishedfileid" "123"  …  "disabled_locally" "0"
        std::string id;
        for (auto& line : split(vdf, '\n')) {
            std::string t = trim(line);
            auto value = [&] {
                size_t b = t.rfind('"'), a = b == std::string::npos || b == 0 ? std::string::npos : t.rfind('"', b - 1);
                return a == std::string::npos ? std::string() : t.substr(a + 1, b - a - 1);
            };
            if (starts_with(t, "\"publishedfileid\"")) {
                id = value();
                out.insert(id);
            } else if (starts_with(t, "\"disabled_locally\"") && value() == "1" && !id.empty()) {
                out.erase(id);
            }
        }
    }
    closedir(d);
    return any;
}

void read_dir(const std::string& dir, std::vector<Item>& out, const std::function<bool(const std::string& name)>& keep) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || !keep(e->d_name)) continue;
        Item it;
        if (read_item(dir + "/" + e->d_name, it)) out.push_back(std::move(it));
    }
    closedir(d);
}

}  // namespace

std::vector<Item> items() {
    std::vector<Item> out;
    std::set<std::string> subs;
    time_t since = 0;
    bool known = false;
    for (auto& lib : steam_libraries()) known |= subscriptions(lib, subs, since);
    for (auto& lib : steam_libraries()) {
        // Workshop downloads you are still subscribed to; one newer than the
        // list (just downloaded, the list not rewritten yet) counts too.
        std::string ws = lib + "/steamapps/workshop/content/431960";
        read_dir(ws, out, [&](const std::string& name) {
            if (!known || subs.count(name)) return true;
            struct stat st {};
            return stat((ws + "/" + name).c_str(), &st) == 0 && st.st_ctime > since;
        });
        // Wallpaper Engine's own: your projects, and the bundled ones it shows
        // (visibility.json hides some, false by name)
        std::string we = lib + "/steamapps/common/wallpaper_engine/projects";
        read_dir(we + "/myprojects", out, [](const std::string&) { return true; });
        std::string vis;
        auto hidden = nlohmann::json::object();
        if (read_file(we + "/defaultprojects/visibility.json", vis)) {
            auto j = nlohmann::json::parse(vis, nullptr, false);
            if (j.is_object()) hidden = std::move(j);
        }
        read_dir(we + "/defaultprojects", out, [&](const std::string& name) {
            auto it = hidden.find(name);
            return it == hidden.end() || !it->is_boolean() || it->get<bool>();
        });
    }
    return out;
}

int clean_workshop() {
    int removed = 0;
    for (auto& lib : steam_libraries()) {
        std::set<std::string> subs;
        time_t since = 0;
        // no list (or an empty one): nothing is known, nothing goes
        if (!subscriptions(lib, subs, since) || subs.empty() || since <= 0) continue;
        std::string ws = lib + "/steamapps/workshop/content/431960";
        DIR* d = opendir(ws.c_str());
        if (!d) continue;
        std::vector<std::string> gone;
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos || subs.count(name)) continue;
            struct stat st {};
            // older than Steam's last rewrite of the list: really unsubscribed,
            // not a download the list has yet to catch up with
            if (stat((ws + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_ctime + 60 < since)
                gone.push_back(ws + "/" + name);
        }
        closedir(d);
        for (auto& dir : gone) {
            nftw(dir.c_str(), [](const char* p, const struct stat*, int, FTW*) { return remove(p); }, 16,
                 FTW_DEPTH | FTW_PHYS);
            if (!file_exists(dir)) ++removed;
        }
    }
    return removed;
}

namespace {

std::string values_path() { return config_dir() + "/wallpaper-properties.json"; }

nlohmann::json load_values() {
    std::string text;
    if (!read_file(values_path(), text)) return nlohmann::json::object();
    auto j = nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

}  // namespace

std::vector<Property> properties(const std::string& dir) {
    std::vector<Property> out;
    std::string text;
    if (!read_file(dir + "/project.json", text)) return out;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("general") || !j["general"].is_object()) return out;
    const auto& props = j["general"].value("properties", nlohmann::json::object());
    if (!props.is_object()) return out;
    std::vector<std::pair<double, Property>> ordered;
    for (auto& [name, e] : props.items()) {
        if (!e.is_object()) continue;
        Property p;
        p.name = name;
        p.type = lower(e.value("type", ""));
        p.text = e.value("text", name);
        if (p.type != "slider" && p.type != "bool" && p.type != "combo" && p.type != "color") continue;
        if (starts_with(p.text, "ui_")) continue;  // Wallpaper Engine's own (scheme colour…)
        p.value = e.contains("value") ? e["value"] : nlohmann::json();
        p.min = e.value("min", 0.0);
        p.max = e.value("max", 1.0);
        p.step = e.value("step", 0.0);
        p.condition = e.value("condition", "");
        if (e.contains("options") && e["options"].is_array())
            for (auto& o : e["options"])
                if (o.is_object()) p.options.push_back({o.value("label", ""), o.contains("value") ? o["value"] : nlohmann::json()});
        if (p.type == "combo" && p.options.empty()) continue;
        // (labels can carry markup: keep the words)
        std::string clean;
        bool tag = false;
        for (char ch : p.text) {
            if (ch == '<') tag = true;
            else if (ch == '>') tag = false;
            else if (!tag) clean += ch;
        }
        p.text = trim(clean).empty() ? name : trim(clean);
        ordered.push_back({e.value("order", 0.0), std::move(p)});
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& [_, p] : ordered) out.push_back(std::move(p));
    return out;
}

nlohmann::json user_values(const std::string& dir) {
    auto all = load_values();
    return all.contains(dir) && all[dir].is_object() ? all[dir] : nlohmann::json::object();
}

void set_user_value(const std::string& dir, const std::string& name, const nlohmann::json& value) {
    auto all = load_values();
    if (!all.contains(dir) || !all[dir].is_object()) all[dir] = nlohmann::json::object();
    all[dir][name] = value;
    write_file_atomic(values_path(), all.dump(1));
}

nlohmann::json value_of(const Property& p, const nlohmann::json& user) {
    return user.contains(p.name) ? user[p.name] : p.value;
}

bool visible(const Property& p, const std::vector<Property>& all, const nlohmann::json& user) {
    // "<name>.value == <literal>" or "!=": anything else shows
    std::string c = trim(p.condition);
    if (c.empty()) return true;
    bool negate = false;
    size_t op = c.find("==");
    if (op == std::string::npos) {
        op = c.find("!=");
        negate = true;
    }
    if (op == std::string::npos) return true;
    std::string lhs = trim(c.substr(0, op)), rhs = trim(c.substr(op + 2));
    if (!ends_with(lhs, ".value")) return true;
    lhs = lhs.substr(0, lhs.size() - 6);
    for (auto& q : all) {
        if (q.name != lhs) continue;
        nlohmann::json v = value_of(q, user);
        std::string have = v.is_string() ? v.get<std::string>() : v.dump();
        if (rhs.size() >= 2 && (rhs.front() == '\'' || rhs.front() == '"')) rhs = rhs.substr(1, rhs.size() - 2);
        bool equal = have == rhs;
        if (v.is_number() && !rhs.empty()) {
            char* end = nullptr;
            double r = strtod(rhs.c_str(), &end);
            if (end && *end == 0) equal = std::abs(v.get<double>() - r) < 1e-9;
        }
        return negate ? !equal : equal;
    }
    return true;
}

std::string engine_overrides(const std::string& dir) {
    auto user = user_values(dir);
    if (user.empty()) return "";
    nlohmann::json out = nlohmann::json::object();
    for (auto& p : properties(dir)) {
        if (!user.contains(p.name)) continue;
        const auto& v = user[p.name];
        if (p.type == "bool") out[p.name] = v.is_boolean() ? (v.get<bool>() ? 1 : 0) : (v.is_number() ? v.get<double>() : 0.0);
        else if (p.type == "combo") {
            bool numeric = v.is_number();
            if (numeric) out[p.name] = v;
            else
                for (size_t i = 0; i < p.options.size(); ++i)
                    if (p.options[i].second == v) out[p.name] = static_cast<int>(i);
        } else if (p.type == "color" && v.is_string()) out[p.name] = v;
        else if (v.is_number()) out[p.name] = v;
    }
    return out.empty() ? "" : out.dump();
}

}  // namespace island::workshop
