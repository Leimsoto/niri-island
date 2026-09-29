#pragma once

#include <string>
#include <functional>

namespace island {

class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    bool start();
    void stop();
    int get_fd() const { return listen_fd_; }

    void process_pending(const std::function<std::string(const std::string& cmd)>& handler);

    static std::string get_socket_path();
    static bool send_client_command(const std::string& cmd, std::string& reply);

private:
    int listen_fd_ = -1;
    std::string socket_path_;
};

} // namespace island
