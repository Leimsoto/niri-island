#include "core/ipc.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

std::string IpcServer::socket_path() { return runtime_dir() + "/niri-island.sock"; }

static int connect_socket(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool IpcServer::instance_running() {
    int fd = connect_socket(socket_path());
    if (fd < 0) return false;
    close(fd);
    return true;
}

IpcServer::~IpcServer() { stop(); }

bool IpcServer::start(std::function<std::string(const std::string&)> handler) {
    handler_ = std::move(handler);
    path_ = socket_path();
    if (instance_running()) return false;
    unlink(path_.c_str());  // stale socket from a crash

    fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || listen(fd_, 16) < 0) {
        std::cerr << "[ipc] cannot listen on " << path_ << ": " << strerror(errno) << "\n";
        close(fd_);
        fd_ = -1;
        return false;
    }
    Loop::get().watch(fd_, POLLIN, [this](short) { accept_clients(); });
    return true;
}

void IpcServer::stop() {
    if (fd_ >= 0) {
        if (Loop* l = &Loop::get()) l->unwatch(fd_);
        close(fd_);
        fd_ = -1;
        unlink(path_.c_str());
    }
}

void IpcServer::accept_clients() {
    for (;;) {
        int c = accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) break;
        // Read one line, but never let a silent client stall the loop.
        std::string cmd;
        char buf[1024];
        double deadline = now_mono() + 0.25;
        while (cmd.find('\n') == std::string::npos && cmd.size() < 65536 && now_mono() < deadline) {
            pollfd p{c, POLLIN, 0};
            if (poll(&p, 1, 50) <= 0) continue;
            ssize_t n = read(c, buf, sizeof(buf));
            if (n <= 0) break;
            cmd.append(buf, static_cast<size_t>(n));
        }
        size_t nl = cmd.find('\n');
        if (nl != std::string::npos) cmd.resize(nl);
        cmd = trim(cmd);

        std::string reply = cmd.empty() ? "error: empty command" : handler_(cmd);
        if (reply.empty()) reply = "ok";
        if (reply.back() != '\n') reply += '\n';
        const char* p = reply.data();
        size_t left = reply.size();
        while (left > 0) {
            ssize_t w = ::send(c, p, left, MSG_NOSIGNAL);
            if (w < 0 && errno == EAGAIN) {
                pollfd pw{c, POLLOUT, 0};
                if (poll(&pw, 1, 100) <= 0) break;
                continue;
            }
            if (w <= 0) break;
            p += w;
            left -= static_cast<size_t>(w);
        }
        close(c);
    }
}

bool IpcServer::send(const std::string& cmd, std::string& reply) {
    int fd = connect_socket(socket_path());
    if (fd < 0) return false;
    std::string payload = cmd + "\n";
    if (write(fd, payload.data(), payload.size()) < 0) {
        close(fd);
        return false;
    }
    shutdown(fd, SHUT_WR);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) reply.append(buf, static_cast<size_t>(n));
    close(fd);
    return true;
}

}  // namespace island
