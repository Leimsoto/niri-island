#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>

#include "core/proc.hpp"

namespace island {

// The Steam Workshop through niri-island-steam (engine/crates/island-steam):
// started when the picker opens the Workshop, gone when it closes. Requests
// and events are JSON lines; events arrive on the main loop.
class Steam {
public:
    ~Steam() { stop(); }
    static bool installed();
    bool running() const { return child_.pid > 0; }
    bool start();  // false: no helper
    void stop();   // closes its stdin: it exits by itself

    void search(const std::string& query, int sort, int page, bool mature, const std::string& tag);
    void subscribe(const std::string& id);    // and download: progress, then done
    void unsubscribe(const std::string& id);  // and delete the download

    // {"ev": ready | results | progress | done | unsubscribed | state | error, …}
    std::function<void(const nlohmann::json&)> on_event;
    std::function<void()> on_exit;  // it went (Steam closed, crashed, or stopped)

private:
    proc::Child child_;
    int in_ = -1, out_ = -1;
    std::string buf_;
    void send(const nlohmann::json& j);
    void readable();
    void exited();
};

}  // namespace island
