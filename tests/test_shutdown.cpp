// Closing down with a download in flight: the transfer gives up quickly and
// the thread's late message to the (gone) loop is dropped, not a crash.

#include <atomic>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "check.hpp"
#include "core/loop.hpp"
#include "services/images.hpp"

using namespace island;

TEST(shutdown_with_a_download_in_flight) {
    // a server that accepts and never answers
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(srv, reinterpret_cast<sockaddr*>(&a), &len);
    listen(srv, 4);
    std::string url = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port)) + "/";

    std::atomic<bool> finished{false}, ran{false};
    auto t0 = std::chrono::steady_clock::now();
    {
        Loop loop;
        std::thread([url, &finished, &ran] {
            std::string body;
            http_get(url, body, 20);  // would wait 20 s if nobody stopped it
            Loop::post_if_alive([&ran] { ran = true; });
            finished = true;
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }  // the loop goes, with the transfer still waiting
    for (int i = 0; i < 50 && !finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(finished);
    CHECK(secs < 3.0);  // gave up, did not sit out its 20 s
    CHECK(!ran);        // its message to the gone loop was dropped
    close(srv);
}
