#include "services/engine.hpp"

#include <climits>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "core/proc.hpp"
#include "core/util.hpp"

namespace island::engine {

namespace {

// Next to our own binary first (a build tree, an install prefix), then PATH.
const std::string& binary() {
    static const std::string path = [] {
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            std::string dir(self, static_cast<size_t>(n));
            dir = dir.substr(0, dir.rfind('/'));
            for (std::string cand : {dir + "/niri-island-engine", dir + "/engine/release/niri-island-engine"})
                if (access(cand.c_str(), X_OK) == 0) return cand;
        }
        return proc::has("niri-island-engine") ? std::string("niri-island-engine") : std::string();
    }();
    return path;
}

proc::Result run(std::vector<std::string> args, int timeout_ms = 20000) {
    args.insert(args.begin(), binary());
    return proc::run(args, timeout_ms, nullptr, true);
}

// Its errors come as JSON or log lines: the last line says enough.
std::string last_line(const std::string& out) {
    std::string t = trim(out);
    size_t nl = t.rfind('\n');
    return nl == std::string::npos ? t : t.substr(nl + 1);
}

}  // namespace

bool installed() { return !binary().empty(); }

// Its server answers on this socket while it shows something. (Any command
// starts the server: the ones that only make sense with it running check first.)
bool running() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) return false;
    std::string path = std::string(rt) + "/niri-island-engine/engine.sock";
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    close(fd);
    return ok;
}

bool apply(const std::string& path, Kind kind, bool muted, std::string* error, const std::string& properties) {
    if (!installed()) {
        if (error) *error = "Falta niri-island-engine";
        return false;
    }
    std::vector<std::string> args = {"apply", "*", path, "--kind", kind == Kind::Engine ? "we" : "video",
                                     "--mute", muted ? "true" : "false", "--replace-all"};
    if (!properties.empty()) {
        args.push_back("--properties");
        args.push_back(properties);
    }
    auto r = run(args, 30000);
    if (!r.ok() && error) *error = last_line(r.out).empty() ? "El motor no pudo mostrar el fondo" : last_line(r.out);
    return r.ok();
}

void stop() {
    if (installed() && running()) run({"stop"}, 8000);
}

bool set_properties(const std::string& properties) {
    if (!installed() || !running() || properties.empty()) return false;
    return run({"properties", properties}, 8000).ok();
}

void set_paused(bool paused) {
    if (installed() && running()) run({paused ? "pause" : "resume"}, 8000);
}

void set_muted(bool muted) {
    if (installed() && running()) run({"audio", "--mute", muted ? "true" : "false"}, 8000);
}

bool capture(const std::string& source, const std::string& png) {
    if (!installed()) return false;
    nlohmann::json req = {{"source", source}, {"destination", png}};
    std::string in = req.dump() + "\n";
    auto r = proc::run({binary(), "capture-scenes"}, 30000, &in);
    return r.ok() && file_exists(png);
}

bool renders(const std::string& type) { return type == "scene" || type == "video"; }

}  // namespace island::engine
