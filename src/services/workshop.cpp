#include "services/workshop.hpp"

#include <algorithm>
#include <dirent.h>
#include <ftw.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <sys/stat.h>

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

}  // namespace island::workshop
