// The greeter's greetd client against a pretend greetd on a socket.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "check.hpp"
#include "greeter/greetd.hpp"

using namespace island;
using json = nlohmann::json;

namespace {

bool read_n(int fd, void* p, size_t n) {
    auto* c = static_cast<char*>(p);
    while (n) {
        ssize_t r = read(fd, c, n);
        if (r <= 0) return false;
        c += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

void send_msg(int fd, const json& j) {
    std::string b = j.dump();
    uint32_t len = static_cast<uint32_t>(b.size());
    (void)!write(fd, &len, 4);
    (void)!write(fd, b.data(), b.size());
}

// Accepts one connection; the right password is "secreto". Records what came.
struct FakeGreetd {
    std::string path;
    int srv = -1;
    std::thread th;
    std::vector<std::string> seen;
    FakeGreetd() {
        char tmpl[] = "/tmp/niri-island-greetd-XXXXXX";
        path = std::string(mkdtemp(tmpl)) + "/sock";
        srv = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path.c_str(), sizeof(a.sun_path) - 1);
        bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(srv, 1);
        th = std::thread([this] {
            int c = accept(srv, nullptr, nullptr);
            uint32_t len;
            while (read_n(c, &len, 4)) {
                std::string b(len, '\0');
                if (!read_n(c, b.data(), len)) break;
                json r = json::parse(b);
                std::string t = r["type"];
                seen.push_back(t);
                if (t == "create_session") send_msg(c, {{"type", "auth_message"}, {"auth_message_type", "secret"}, {"auth_message", "Password:"}});
                else if (t == "post_auth_message_response")
                    send_msg(c, r["response"] == "secreto" ? json{{"type", "success"}}
                                                           : json{{"type", "error"}, {"error_type", "auth_error"}, {"description", "x"}});
                else send_msg(c, {{"type", "success"}});
            }
            close(c);
        });
    }
    ~FakeGreetd() {
        th.join();
        close(srv);
        unlink(path.c_str());
        rmdir(path.substr(0, path.rfind('/')).c_str());
    }
};

}  // namespace

TEST(greetd_login_ok) {
    std::vector<std::string> seen;
    std::string err;
    {
        FakeGreetd g;
        err = greetd_login(g.path, "ana", "secreto", {"niri-session"}, {});
        seen = std::move(g.seen);  // (the server thread is joined below)
    }
    CHECK_EQ(err, std::string(""));
    CHECK(seen.size() >= 3 && seen.back() == "start_session");
}

TEST(greetd_wrong_password) {
    std::vector<std::string> seen;
    std::string err;
    {
        FakeGreetd g;
        err = greetd_login(g.path, "ana", "mal", {"niri-session"}, {});
        seen = g.seen;
    }
    CHECK_EQ(err, std::string("Contraseña incorrecta"));
    CHECK(std::find(seen.begin(), seen.end(), "cancel_session") != seen.end());
    CHECK(std::find(seen.begin(), seen.end(), "start_session") == seen.end());
}

TEST(greetd_missing_socket) {
    CHECK(!greetd_login("/nonexistent/sock", "ana", "x", {"sh"}, {}).empty());
    CHECK(!greetd_login("", "ana", "x", {"sh"}, {}).empty());
}
