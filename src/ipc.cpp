#include "ipc.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstdlib>

namespace island {

std::string IpcServer::get_socket_path() {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && runtime_dir[0] != '\0') {
        return std::string(runtime_dir) + "/niri-island.sock";
    }
    return "/tmp/niri-island-" + std::to_string(getuid()) + ".sock";
}

IpcServer::IpcServer() = default;

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    socket_path_ = get_socket_path();
    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[IPC] Failed to create socket\n";
        return false;
    }

    // Set non-blocking
    int flags = fcntl(listen_fd_, F_GETFL, 0);
    fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[IPC] Failed to bind socket to " << socket_path_ << "\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 5) < 0) {
        std::cerr << "[IPC] Failed to listen on socket\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    std::cout << "[IPC] Listening on " << socket_path_ << "\n";
    return true;
}

void IpcServer::stop() {
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
    }
}

void IpcServer::process_pending(const std::function<std::string(const std::string&)>& handler) {
    if (listen_fd_ < 0) return;

    while (true) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) break;

        char buf[256] = {0};
        ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            std::string cmd(buf);
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();

            std::string reply = handler ? handler(cmd) : "ok";
            if (reply.empty()) reply = "ok";
            if (reply.back() != '\n') reply += "\n";

            ssize_t w = write(client_fd, reply.data(), reply.size());
            (void)w;
        }
        close(client_fd);
    }
}

bool IpcServer::send_client_command(const std::string& cmd, std::string& reply) {
    std::string path = get_socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    std::string payload = cmd + "\n";
    ssize_t w = write(fd, payload.data(), payload.size());
    (void)w;

    char buf[1024] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    if (r > 0) {
        reply = std::string(buf, r);
    }
    close(fd);
    return true;
}

} // namespace island
