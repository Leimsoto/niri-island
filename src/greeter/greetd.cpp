#include "greeter/greetd.hpp"

#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace island {

using json = nlohmann::json;

namespace {

class Conn {
public:
    explicit Conn(const std::string& path) {
        fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            close_fd();
            return;
        }
        memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) close_fd();
    }
    ~Conn() { close_fd(); }
    bool ok() const { return fd_ >= 0; }

    // One request, one reply.
    bool call(const json& req, json& reply) {
        std::string body = req.dump();
        uint32_t len = static_cast<uint32_t>(body.size());
        if (!write_all(&len, sizeof(len)) || !write_all(body.data(), body.size())) return false;
        if (!read_all(&len, sizeof(len)) || len > (1u << 20)) return false;
        std::string in(len, '\0');
        if (!read_all(in.data(), len)) return false;
        reply = json::parse(in, nullptr, false);
        // nothing secret stays around in our copy of the request
        std::fill(body.begin(), body.end(), '\0');
        return !reply.is_discarded();
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
};

std::string describe(const json& r) {
    std::string type = r.value("error_type", "");
    if (type == "auth_error") return "Contraseña incorrecta";
    std::string d = r.value("description", "");
    return d.empty() ? "No se pudo iniciar la sesión" : d;
}

}  // namespace

std::string greetd_login(const std::string& socket_path, const std::string& user, const std::string& password,
                         const std::vector<std::string>& cmd, const std::vector<std::string>& env) {
    if (socket_path.empty()) return "greetd no está disponible (GREETD_SOCK)";
    Conn c(socket_path);
    if (!c.ok()) return "No se pudo conectar con greetd";
    json r;
    if (!c.call({{"type", "create_session"}, {"username", user}}, r)) return "greetd no responde";
    // A session left half-way by an earlier try: start over.
    if (r.value("type", "") == "error") {
        json ignore;
        c.call({{"type", "cancel_session"}}, ignore);
        if (!c.call({{"type", "create_session"}, {"username", user}}, r)) return "greetd no responde";
    }
    // PAM's prompts: the password for secret/visible ones; info and errors
    // just get acknowledged. A few rounds at most (password, then maybe an
    // account notice).
    for (int round = 0; round < 8 && r.value("type", "") == "auth_message"; ++round) {
        std::string kind = r.value("auth_message_type", "");
        json req = {{"type", "post_auth_message_response"}};
        if (kind == "secret" || kind == "visible") req["response"] = password;
        else req["response"] = nullptr;
        if (!c.call(req, r)) return "greetd no responde";
    }
    std::string type = r.value("type", "");
    if (type == "error") {
        json ignore;
        c.call({{"type", "cancel_session"}}, ignore);
        return describe(r);
    }
    if (type != "success") return "Respuesta inesperada de greetd";
    if (!c.call({{"type", "start_session"}, {"cmd", cmd}, {"env", env}}, r)) return "greetd no responde";
    if (r.value("type", "") != "success") {
        json ignore;
        c.call({{"type", "cancel_session"}}, ignore);
        return describe(r);
    }
    return "";
}

}  // namespace island
