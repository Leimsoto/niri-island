#include "services/niri.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

using json = nlohmann::json;

static int connect_unix(const std::string& path, bool nonblock) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (nonblock) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

Niri::~Niri() { disconnect(); }

bool Niri::init() {
    const char* s = getenv("NIRI_SOCKET");
    if (!s || !*s) {
        std::cerr << "[niri] NIRI_SOCKET is not set; workspaces disabled\n";
        return false;
    }
    path_ = s;
    connect_stream();
    return fd_ >= 0;
}

void Niri::disconnect() {
    if (fd_ >= 0) {
        Loop::get().unwatch(fd_);
        close(fd_);
        fd_ = -1;
    }
}

void Niri::connect_stream() {
    disconnect();
    int fd = connect_unix(path_, false);
    if (fd < 0) {
        // niri not up yet (or restarting): retry with backoff.
        Loop::get().cancel(retry_timer_);
        retry_timer_ = Loop::get().after(backoff_, [this] {
            retry_timer_ = 0;
            connect_stream();
        });
        backoff_ = std::min(backoff_ * 2.0, 10.0);
        return;
    }
    const char* req = "\"EventStream\"\n";
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_ = fd;
    backoff_ = 1.0;
    buf_.clear();
    Loop::get().watch(fd_, POLLIN, [this](short ev) {
        bool eof = readable();
        if (eof || (ev & (POLLHUP | POLLERR))) {
            // The old code spun at 100% CPU on EOF. Reconnect instead.
            std::cerr << "[niri] event stream closed, reconnecting\n";
            connect_stream();
        }
    });
}

bool Niri::readable() {
    bool eof = false;
    char tmp[16384];
    for (;;) {
        ssize_t n = read(fd_, tmp, sizeof(tmp));
        if (n > 0) {
            buf_.append(tmp, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            eof = true;
            break;
        }
        if (errno == EINTR) continue;
        break;  // EAGAIN
    }
    size_t pos;
    bool changed = false;
    while ((pos = buf_.find('\n')) != std::string::npos) {
        std::string line = buf_.substr(0, pos);
        buf_.erase(0, pos + 1);
        if (!line.empty()) {
            handle(line);
            changed = true;
        }
    }
    if (buf_.size() > (4u << 20)) buf_.clear();
    if (changed && on_change) on_change();
    return eof;
}

static Workspace parse_ws(const json& w) {
    Workspace ws;
    ws.id = w.value("id", (uint64_t)0);
    ws.idx = w.value("idx", 0);
    if (w.contains("name") && w["name"].is_string()) ws.name = w["name"];
    if (w.contains("output") && w["output"].is_string()) ws.output = w["output"];
    ws.active = w.value("is_active", false);
    ws.focused = w.value("is_focused", false);
    ws.urgent = w.value("is_urgent", false);
    if (w.contains("active_window_id") && w["active_window_id"].is_number())
        ws.active_window = w["active_window_id"].get<uint64_t>();
    return ws;
}

static Window parse_win(const json& w) {
    Window win;
    win.id = w.value("id", (uint64_t)0);
    if (w.contains("title") && w["title"].is_string()) win.title = w["title"];
    if (w.contains("app_id") && w["app_id"].is_string()) win.app_id = w["app_id"];
    if (w.contains("workspace_id") && w["workspace_id"].is_number()) win.workspace = w["workspace_id"].get<uint64_t>();
    win.focused = w.value("is_focused", false);
    win.urgent = w.value("is_urgent", false);
    return win;
}

void Niri::handle(const std::string& line) {
    json j = json::parse(line, nullptr, false);
    if (!j.is_object() || j.empty()) return;
    const std::string key = j.begin().key();
    const json& v = j.begin().value();
    if (key == "WorkspacesChanged") {
        workspaces_.clear();
        for (auto& w : v.value("workspaces", json::array())) {
            Workspace ws = parse_ws(w);
            workspaces_[ws.id] = ws;
        }
    } else if (key == "WorkspaceActivated") {
        uint64_t id = v.value("id", (uint64_t)0);
        bool focused = v.value("focused", false);
        auto it = workspaces_.find(id);
        if (it != workspaces_.end()) {
            std::string out = it->second.output;
            for (auto& [wid, ws] : workspaces_) {
                if (ws.output == out) ws.active = wid == id;
                if (focused) ws.focused = wid == id;
            }
        }
    } else if (key == "WorkspaceUrgencyChanged") {
        auto it = workspaces_.find(v.value("id", (uint64_t)0));
        if (it != workspaces_.end()) it->second.urgent = v.value("urgent", false);
    } else if (key == "WorkspaceActiveWindowChanged") {
        auto it = workspaces_.find(v.value("workspace_id", (uint64_t)0));
        if (it != workspaces_.end())
            it->second.active_window = v.contains("active_window_id") && v["active_window_id"].is_number()
                                           ? v["active_window_id"].get<uint64_t>()
                                           : 0;
    } else if (key == "WindowsChanged") {
        windows_.clear();
        for (auto& w : v.value("windows", json::array())) {
            Window win = parse_win(w);
            windows_[win.id] = win;
        }
    } else if (key == "WindowOpenedOrChanged") {
        if (v.contains("window")) {
            Window win = parse_win(v["window"]);
            if (win.focused)
                for (auto& [id, w] : windows_) w.focused = false;
            windows_[win.id] = win;
        }
    } else if (key == "WindowClosed") {
        windows_.erase(v.value("id", (uint64_t)0));
    } else if (key == "WindowFocusChanged") {
        uint64_t id = v.contains("id") && v["id"].is_number() ? v["id"].get<uint64_t>() : 0;
        for (auto& [wid, w] : windows_) w.focused = wid == id;
    } else if (key == "WindowUrgencyChanged") {
        auto it = windows_.find(v.value("id", (uint64_t)0));
        if (it != windows_.end()) it->second.urgent = v.value("urgent", false);
    } else if (key == "OverviewOpenedOrClosed") {
        overview_ = v.value("is_open", false);
    }
}

std::vector<Workspace> Niri::output_workspaces() const {
    std::string out;
    for (auto& [id, ws] : workspaces_)
        if (ws.focused) out = ws.output;
    std::vector<Workspace> list;
    for (auto& [id, ws] : workspaces_)
        if (out.empty() || ws.output == out) list.push_back(ws);
    std::sort(list.begin(), list.end(), [](const Workspace& a, const Workspace& b) { return a.idx < b.idx; });
    return list;
}

int Niri::active_slot() const {
    auto list = output_workspaces();
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].active) return static_cast<int>(i);
    return 0;
}

const Window* Niri::focused_window() const {
    for (auto& [id, w] : windows_)
        if (w.focused) return &w;
    return nullptr;
}

void Niri::action(const std::string& payload) {
    std::string path = path_;
    Loop::get().async([path, payload] {
        int fd = connect_unix(path, false);
        if (fd < 0) return;
        std::string msg = payload + "\n";
        if (write(fd, msg.data(), msg.size()) > 0) {
            // Read the reply so niri doesn't log a broken pipe.
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 1000) > 0) {
                char b[512];
                ssize_t r = read(fd, b, sizeof(b));
                (void)r;
            }
        }
        close(fd);
    });
}

void Niri::focus_workspace(uint64_t id) {
    // Optimistic: move the pill now, niri confirms a moment later.
    auto it = workspaces_.find(id);
    if (it != workspaces_.end()) {
        for (auto& [wid, ws] : workspaces_)
            if (ws.output == it->second.output) ws.active = wid == id;
        if (on_change) on_change();
    }
    action(json{{"Action", {{"FocusWorkspace", {{"reference", {{"Id", id}}}}}}}}.dump());
}

void Niri::focus_workspace_relative(int delta) {
    action(json{{"Action", {{delta > 0 ? "FocusWorkspaceDown" : "FocusWorkspaceUp", json::object()}}}}.dump());
}

void Niri::focus_window(uint64_t id) { action(json{{"Action", {{"FocusWindow", {{"id", id}}}}}}.dump()); }

bool Niri::focus_app(const std::string& hint) {
    if (hint.empty()) return false;
    std::string h = lower(hint);
    if (ends_with(h, ".desktop")) h = h.substr(0, h.size() - 8);
    std::string first = h.substr(0, h.find(' '));
    const Window* best = nullptr;
    for (auto& [id, w] : windows_) {
        std::string app = lower(w.app_id), title = lower(w.title);
        if (app == h || ends_with(app, "." + h) || (!first.empty() && first.size() >= 3 && app.find(first) != std::string::npos)) {
            best = &w;
            break;
        }
        if (!best && first.size() >= 4 && title.find(first) != std::string::npos) best = &w;
    }
    if (!best) return false;
    focus_window(best->id);
    return true;
}

}  // namespace island
