#include "workspaces.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstdlib>
#include <algorithm>
#include <sstream>

namespace island {

WorkspaceManager::WorkspaceManager() = default;

WorkspaceManager::~WorkspaceManager() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
    }
}

bool WorkspaceManager::init() {
    const char* env_socket = std::getenv("NIRI_SOCKET");
    if (env_socket && env_socket[0] != '\0') {
        socket_path_ = env_socket;
    } else {
        // Fallback search in /run/user/<uid>/
        const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
        if (runtime_dir) {
            std::string pattern = std::string(runtime_dir) + "/niri.wayland-1.sock";
            socket_path_ = pattern;
        }
    }

    event_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (event_fd_ >= 0) {
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(event_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            const char* req = "\"EventStream\"\n";
            if (write(event_fd_, req, strlen(req)) > 0) {
                int flags = fcntl(event_fd_, F_GETFL, 0);
                fcntl(event_fd_, F_SETFL, flags | O_NONBLOCK);
            }
        } else {
            close(event_fd_);
            event_fd_ = -1;
        }
    }

    query_workspaces();
    return !workspaces_.empty();
}

void WorkspaceManager::handle_event() {
    if (event_fd_ < 0) return;
    char buf[2048];
    while (read(event_fd_, buf, sizeof(buf)) > 0) {}
    last_poll_time_ = std::chrono::steady_clock::time_point{}; // force query
    query_workspaces();
}

float WorkspaceManager::bubble_width() const {
    size_t count = workspaces_.empty() ? 4 : workspaces_.size();
    if (count < 1) count = 1;
    // 14px padding each side + 20px spacing between slot centers
    float w = 28.0f + static_cast<float>(count - 1) * 20.0f;
    return std::max(76.0f, w);
}

int WorkspaceManager::active_slot() const {
    for (size_t i = 0; i < workspaces_.size(); ++i) {
        if (workspaces_[i].idx == active_idx_ || workspaces_[i].is_active || workspaces_[i].is_focused) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

void WorkspaceManager::query_workspaces() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - last_poll_time_).count();
    if (elapsed < 0.25f && !workspaces_.empty()) {
        return;
    }
    last_poll_time_ = now;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        // Fallback to niri CLI if direct socket connect fails
        FILE* fp = popen("niri msg --json workspaces 2>/dev/null", "r");
        if (fp) {
            std::string out;
            char buf[512];
            while (fgets(buf, sizeof(buf), fp)) {
                out += buf;
            }
            pclose(fp);
            if (!out.empty()) {
                parse_workspaces_json(out);
            }
        }
        return;
    }

    const char* req = "\"Workspaces\"\n";
    if (write(fd, req, strlen(req)) < 0) {
        close(fd);
        return;
    }

    std::string response;
    char buffer[1024];
    ssize_t n;
    while ((n = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[n] = '\0';
        response += buffer;
        if (response.find('\n') != std::string::npos) break;
    }
    close(fd);

    if (!response.empty()) {
        parse_workspaces_json(response);
    }
}

void WorkspaceManager::parse_workspaces_json(const std::string& text) {
    std::vector<WorkspaceInfo> list;

    size_t pos = 0;
    while ((pos = text.find('{', pos)) != std::string::npos) {
        size_t end = text.find('}', pos);
        if (end == std::string::npos) break;
        std::string obj = text.substr(pos, end - pos + 1);
        pos = end + 1;

        if (obj.find("\"idx\":") == std::string::npos) continue;

        WorkspaceInfo info;
        size_t idx_pos = obj.find("\"idx\":");
        if (idx_pos != std::string::npos) {
            info.idx = std::atoi(obj.c_str() + idx_pos + 6);
        }

        size_t id_pos = obj.find("\"id\":");
        if (id_pos != std::string::npos) {
            info.id = std::atoi(obj.c_str() + id_pos + 5);
        }

        size_t act_pos = obj.find("\"is_active\":");
        if (act_pos != std::string::npos) {
            info.is_active = (obj.substr(act_pos + 12, 4) == "true");
        }

        size_t foc_pos = obj.find("\"is_focused\":");
        if (foc_pos != std::string::npos) {
            info.is_focused = (obj.substr(foc_pos + 13, 4) == "true");
        }

        size_t name_pos = obj.find("\"name\":\"");
        if (name_pos != std::string::npos) {
            size_t name_end = obj.find('"', name_pos + 8);
            if (name_end != std::string::npos) {
                info.name = obj.substr(name_pos + 8, name_end - (name_pos + 8));
            }
        }

        if (info.is_active || info.is_focused) {
            active_idx_ = info.idx;
        }

        list.push_back(info);
    }

    if (!list.empty()) {
        std::sort(list.begin(), list.end(), [](const WorkspaceInfo& a, const WorkspaceInfo& b) {
            return a.idx < b.idx;
        });
        workspaces_ = std::move(list);
    }
}

void WorkspaceManager::update() {
    query_workspaces();
}

void WorkspaceManager::focus_workspace(uint32_t idx) {
    std::string cmd = "niri msg action focus-workspace " + std::to_string(idx) + " &";
    int r = system(cmd.c_str());
    (void)r;

    active_idx_ = idx;
    for (auto& ws : workspaces_) {
        ws.is_active = (ws.idx == idx);
        ws.is_focused = (ws.idx == idx);
    }
}

} // namespace island
