#include "services/waywallen.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <random>
#include <functional>
#include <set>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <dirent.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "core/proc.hpp"
#include "core/util.hpp"

namespace island::waywallen {

namespace {

constexpr const char* kFlatpak = "org.waywallen.waywallen";
constexpr const char* kBus = "org.waywallen.waywallen.Daemon";
constexpr const char* kPath = "/org/waywallen/waywallen/Daemon";
constexpr const char* kIface = "org.waywallen.waywallen.Daemon1";
constexpr const char* kSource = "wallpaper_engine";  // the plugin's source name

// Request fields (control.v1 Request oneof)
enum : int { kWallpaperList = 19, kWallpaperScan = 20, kWallpaperApply = 22, kLibraryAdd = 27, kPluginInstall = 46,
             kGlobalPauseSet = 76, kGlobalStopSet = 78 };

// ── protobuf, just enough ──

void put_varint(std::string& out, uint64_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        out.push_back(static_cast<char>(b | (v ? 0x80 : 0)));
    } while (v);
}
void put_field(std::string& out, int field, uint64_t v) {
    put_varint(out, static_cast<uint64_t>(field) << 3);
    put_varint(out, v);
}
void put_field(std::string& out, int field, const std::string& bytes) {
    put_varint(out, static_cast<uint64_t>(field) << 3 | 2);
    put_varint(out, bytes.size());
    out += bytes;
}

struct Field {
    int num = 0, type = 0;
    uint64_t v = 0;
    std::string_view bytes;
};
// Walks a message's fields; false at the end or on garbage.
bool next_field(std::string_view buf, size_t& i, Field& f) {
    auto varint = [&](uint64_t& out) {
        out = 0;
        for (int sh = 0; i < buf.size() && sh < 64; sh += 7) {
            uint8_t b = static_cast<uint8_t>(buf[i++]);
            out |= static_cast<uint64_t>(b & 0x7f) << sh;
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    if (i >= buf.size()) return false;
    uint64_t tag;
    if (!varint(tag)) return false;
    f.num = static_cast<int>(tag >> 3);
    f.type = static_cast<int>(tag & 7);
    switch (f.type) {
        case 0: return varint(f.v);
        case 2: {
            uint64_t len;
            if (!varint(len) || i + len > buf.size()) return false;
            f.bytes = buf.substr(i, len);
            i += len;
            return true;
        }
        case 1: i += 8; return i <= buf.size();
        case 5: i += 4; return i <= buf.size();
        default: return false;
    }
}

// ── the control WebSocket ──

std::string bus_property(const char* name) {
    auto r = proc::run({"busctl", "--user", "get-property", kBus, kPath, kIface, name}, 3000);
    if (!r.ok()) return "";
    std::string s = trim(r.out);  // e.g. `q 42855` or `s "0.5.0"`
    size_t sp = s.find(' ');
    return sp == std::string::npos ? "" : s.substr(sp + 1);
}

class Ws {
public:
    explicit Ws(int port) {
        fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return;
        timeval tv{30, 0};  // applying spawns a renderer: give it time
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(static_cast<uint16_t>(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 || !handshake()) close_fd();
    }
    ~Ws() { close_fd(); }
    bool ok() const { return fd_ >= 0; }

    bool send_binary(const std::string& p) {
        std::string f;
        f.push_back(static_cast<char>(0x82));
        if (p.size() < 126) f.push_back(static_cast<char>(0x80 | p.size()));
        else if (p.size() < 65536) {
            f.push_back(static_cast<char>(0x80 | 126));
            f.push_back(static_cast<char>(p.size() >> 8));
            f.push_back(static_cast<char>(p.size() & 0xff));
        } else {
            f.push_back(static_cast<char>(0x80 | 127));
            for (int k = 7; k >= 0; --k) f.push_back(static_cast<char>((static_cast<uint64_t>(p.size()) >> (k * 8)) & 0xff));
        }
        uint8_t mask[4];
        std::random_device rd;
        for (auto& m : mask) m = static_cast<uint8_t>(rd());
        f.append(reinterpret_cast<char*>(mask), 4);
        for (size_t k = 0; k < p.size(); ++k) f.push_back(static_cast<char>(p[k] ^ mask[k % 4]));
        return write_all(f.data(), f.size());
    }
    // Next data frame (binary or text); answers pings, skips the rest.
    bool recv_frame(std::string& out) {
        for (;;) {
            uint8_t h[2];
            if (!read_all(h, 2)) return false;
            int op = h[0] & 0x0f;
            uint64_t len = h[1] & 0x7f;
            if (len == 126) {
                uint8_t e[2];
                if (!read_all(e, 2)) return false;
                len = (e[0] << 8) | e[1];
            } else if (len == 127) {
                uint8_t e[8];
                if (!read_all(e, 8)) return false;
                len = 0;
                for (uint8_t b : e) len = (len << 8) | b;
            }
            if (len > (64u << 20)) return false;
            std::string data(len, '\0');
            if (len && !read_all(data.data(), len)) return false;
            if (op == 8) return false;  // close
            if (op == 9) {              // ping: pong
                std::string pong;
                pong.push_back(static_cast<char>(0x8a));
                pong.push_back(static_cast<char>(0x80));
                pong.append(4, '\0');
                write_all(pong.data(), pong.size());
                continue;
            }
            if (op == 1 || op == 2) {
                out = std::move(data);
                return true;
            }
        }
    }

private:
    int fd_ = -1;
    void close_fd() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }
    bool write_all(const void* p, size_t n) {
        auto* c = static_cast<const char*>(p);
        while (n) {
            ssize_t w = write(fd_, c, n);
            if (w <= 0) return false;
            c += w;
            n -= static_cast<size_t>(w);
        }
        return true;
    }
    bool read_all(void* p, size_t n) {
        auto* c = static_cast<char*>(p);
        while (n) {
            ssize_t r = read(fd_, c, n);
            if (r <= 0) return false;
            c += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }
    bool handshake() {
        // the key only has to be 16 random bytes in base64; the reply is not checked
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::random_device rd;
        std::string key;
        for (int k = 0; k < 22; ++k) key.push_back(b64[rd() % 64]);
        key += "==";
        std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!write_all(req.data(), req.size())) return false;
        std::string head;
        char ch;
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
            if (read(fd_, &ch, 1) != 1) return false;
            head.push_back(ch);
        }
        return head.find(" 101 ") != std::string::npos;
    }
};

// One request; the matching response's payload (field `field`) in `payload`.
bool request(int field, const std::string& msg, std::string& payload, std::string* error) {
    int port = std::atoi(bus_property("WsPort").c_str());
    if (port <= 0) {
        if (error) *error = "waywallen no está en marcha";
        return false;
    }
    Ws ws(port);
    if (!ws.ok()) {
        if (error) *error = "No se pudo conectar con waywallen";
        return false;
    }
    static uint64_t next_id = 1000;
    uint64_t id = ++next_id;
    std::string req;
    put_field(req, 1, id);
    put_field(req, field, msg);
    if (!ws.send_binary(req)) return false;
    std::string frame;
    while (ws.recv_frame(frame)) {
        // ServerFrame { response = 1 | event = 2 }
        size_t i = 0;
        Field f;
        while (next_field(frame, i, f)) {
            if (f.num != 1 || f.type != 2) continue;
            uint64_t rid = 0, status = 0, code = 0;
            std::string message;
            std::string_view body;
            size_t j = 0;
            Field g;
            while (next_field(f.bytes, j, g)) {
                if (g.num == 1) rid = g.v;
                else if (g.num == 2) status = g.v;
                else if (g.num == 3) message = std::string(g.bytes);
                else if (g.num == 4) code = g.v;
                else if (g.num == field && g.type == 2) body = g.bytes;
            }
            if (rid != id) continue;
            if (code != 0 || (status != 0 && status != 1)) {
                if (error) *error = message.empty() ? "waywallen: error " + std::to_string(code) : message;
                return false;
            }
            payload = std::string(body);
            return true;
        }
    }
    if (error) *error = "waywallen no respondió";
    return false;
}

bool busctl_call(const char* method) {
    return proc::run({"busctl", "--user", "call", kBus, kPath, kIface, method}, 5000).ok();
}

// The layer-shell client (waywallen-display) puts the daemon's frames on
// screen: inside Flatpak the daemon cannot. Its process name is cut to 15.
std::vector<int> display_pids() {
    std::vector<int> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        int pid = std::atoi(e->d_name);
        if (pid <= 0) continue;
        std::string comm;
        if (read_file(std::string("/proc/") + e->d_name + "/comm", comm) && trim(comm) == "waywallen-layer") out.push_back(pid);
    }
    closedir(d);
    return out;
}

bool flatpak_installed() {
    static int cached = -1;
    if (cached < 0) cached = proc::has("flatpak") && proc::run({"flatpak", "info", kFlatpak}, 5000).ok() ? 1 : 0;
    return cached == 1;
}

}  // namespace

bool installed() { return flatpak_installed() || proc::has("waywallen"); }

bool plugin_installed() {
    for (std::string d : {home_dir() + "/.var/app/org.waywallen.waywallen/data/waywallen/plugins",
                          home_dir() + "/.local/share/waywallen/plugins"})
        if (file_exists(d + "/org.waywallen.open-wallpaper-engine")) return true;
    return false;
}

bool running() { return !bus_property("WsPort").empty(); }

bool display_installed() { return proc::has("waywallen-layer-shell"); }

bool ensure_running(int timeout_ms) {
    if (running()) {
        if (display_pids().empty() && display_installed()) proc::spawn({"waywallen-layer-shell"});
        return true;
    }
    if (!installed()) return false;
    if (flatpak_installed()) proc::spawn({"flatpak", "run", kFlatpak, "--no-ui", "--no-tray", "--no-restore"});
    else proc::spawn({"waywallen", "--no-ui", "--no-tray", "--no-restore"});
    for (int waited = 0; waited < timeout_ms; waited += 250) {
        usleep(250000);
        if (std::atoi(bus_property("WsPort").c_str()) > 0) {
            if (display_pids().empty() && display_installed()) proc::spawn({"waywallen-layer-shell"});
            return true;
        }
    }
    return false;
}

void stop_and_quit() {
    for (int pid : display_pids()) kill(pid, SIGTERM);
    if (!running()) return;
    std::string msg, out;
    put_field(msg, 1, 1);  // stopped = true
    request(kGlobalStopSet, msg, out, nullptr);
    busctl_call("Quit");
}

void set_paused(bool paused) {
    if (running()) busctl_call(paused ? "Pause" : "Resume");
}

void set_muted(bool muted) {
    if (running()) busctl_call(muted ? "Mute" : "Unmute");
}

void open_ui() {
    if (running()) busctl_call("OpenUi");
    else if (flatpak_installed()) proc::spawn({"flatpak", "run", kFlatpak, "--ui", "--no-restore"});
    else proc::spawn({"waywallen", "--ui", "--no-restore"});
}

bool list(std::vector<Entry>& out, std::string* error) {
    std::string payload;
    if (!request(kWallpaperList, "", payload, error)) return false;
    size_t i = 0;
    Field f;
    while (next_field(payload, i, f)) {
        if (f.num != 1 || f.type != 2) continue;
        Entry e;
        size_t j = 0;
        Field g;
        while (next_field(f.bytes, j, g)) {
            if (g.type != 2) continue;
            std::string s(g.bytes);
            if (g.num == 1) e.id = s;
            else if (g.num == 2) e.name = s;
            else if (g.num == 3) e.type = s;
            else if (g.num == 4) e.resource = s;
            else if (g.num == 5) e.preview = s;
        }
        if (!e.id.empty()) out.push_back(std::move(e));
    }
    return true;
}

bool scan(std::string* error) {
    std::string out;
    return request(kWallpaperScan, "", out, error);
}

bool apply(const std::string& id, std::string* error) {
    std::string msg, out;
    put_field(msg, 1, id);
    // a display client just started takes a moment to register
    for (int tries = 0;; ++tries) {
        std::string err;
        if (request(kWallpaperApply, msg, out, &err)) return true;
        if (err.find("no display") == std::string::npos || tries >= 20) {
            if (error) *error = err;
            return false;
        }
        usleep(250000);
    }
}

bool apply_dir(const std::string& dir, std::string* error) {
    auto find = [&](std::string& id) {
        std::vector<Entry> all;
        if (!list(all, error)) return false;
        std::string d = dir.back() == '/' ? dir : dir + "/";
        for (auto& e : all)
            if (e.resource == dir || starts_with(e.resource, d)) {
                id = e.id;
                return true;
            }
        return false;
    };
    std::string id;
    if (!find(id)) {
        // a new download: look again once
        if (!scan(error) || !find(id)) {
            if (error && error->empty()) *error = "waywallen no conoce este fondo (¿falta el plugin o la biblioteca?)";
            return false;
        }
    }
    return waywallen::apply(id, error);
}

bool install_plugin(const std::string& zip, std::string* error) {
    std::string msg, out;
    put_field(msg, 1, zip);
    return request(kPluginInstall, msg, out, error);
}

bool add_library(const std::string& steam_root, std::string* error) {
    std::string msg, out;
    put_field(msg, 1, steam_root);
    put_field(msg, 2, std::string(kSource));
    put_field(msg, 3, std::string("Steam"));
    return request(kLibraryAdd, msg, out, error);
}

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

}  // namespace island::waywallen
