#pragma once

#include <functional>
#include <string>

namespace island {

// Line-based command socket at $XDG_RUNTIME_DIR/niri-island.sock.
class IpcServer {
public:
    ~IpcServer();

    // Returns false if another instance already answers on the socket.
    bool start(std::function<std::string(const std::string&)> handler);
    void stop();

    static std::string socket_path();
    static bool send(const std::string& cmd, std::string& reply);
    static bool instance_running();

private:
    int fd_ = -1;
    std::string path_;
    std::function<std::string(const std::string&)> handler_;
    void accept_clients();
};

}  // namespace island
