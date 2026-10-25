#include "services/steam.hpp"

#include <climits>
#include <poll.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

namespace {

// Next to our own binary first (a build tree, an install prefix), then PATH.
std::string helper() {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        std::string dir(self, static_cast<size_t>(n));
        dir = dir.substr(0, dir.rfind('/'));
        for (std::string cand : {dir + "/niri-island-steam", dir + "/engine/release/niri-island-steam"})
            if (access(cand.c_str(), X_OK) == 0) return cand;
    }
    return proc::has("niri-island-steam") ? "niri-island-steam" : "";
}

}  // namespace

bool Steam::installed() { return !helper().empty(); }

bool Steam::start() {
    if (running()) return true;
    std::string bin = helper();
    if (bin.empty()) return false;
    child_ = proc::start_piped({bin}, in_, out_, cache_dir() + "/steam.log");
    if (child_.pid <= 0) return false;
    buf_.clear();
    Loop::get().watch(out_, POLLIN, [this](short) { readable(); });
    if (child_.pidfd >= 0) Loop::get().watch(child_.pidfd, POLLIN, [this](short) { exited(); });
    return true;
}

void Steam::stop() {
    if (!running()) return;
    if (in_ >= 0) close(in_);  // EOF: it leaves on its own
    in_ = -1;
}

void Steam::send(const nlohmann::json& j) {
    if (in_ < 0) return;
    std::string line = j.dump() + "\n";
    // short lines into an empty pipe: they fit whole
    ssize_t r = write(in_, line.data(), line.size());
    (void)r;
}

void Steam::search(const std::string& query, int sort, int page, bool mature, const std::string& tag) {
    send({{"op", "search"}, {"query", query}, {"sort", sort}, {"page", page}, {"mature", mature}, {"tag", tag}});
}

void Steam::subscribe(const std::string& id) { send({{"op", "subscribe"}, {"id", id}}); }

void Steam::unsubscribe(const std::string& id) { send({{"op", "unsubscribe"}, {"id", id}}); }

void Steam::cancel(const std::string& id) { send({{"op", "cancel"}, {"id", id}}); }

void Steam::details(const std::vector<std::string>& ids) { send({{"op", "details"}, {"ids", ids}}); }

void Steam::readable() {
    char chunk[8192];
    for (;;) {
        ssize_t n = read(out_, chunk, sizeof(chunk));
        if (n > 0) {
            buf_.append(chunk, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {  // its end closed
            Loop::get().unwatch(out_);
            close(out_);
            out_ = -1;
        }
        break;
    }
    size_t nl;
    while ((nl = buf_.find('\n')) != std::string::npos) {
        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_object() && on_event) on_event(j);
    }
}

void Steam::exited() {
    if (out_ >= 0) readable();  // whatever it said last
    if (child_.pidfd >= 0) Loop::get().unwatch(child_.pidfd);
    if (out_ >= 0) {
        Loop::get().unwatch(out_);
        close(out_);
        out_ = -1;
    }
    if (in_ >= 0) close(in_);
    in_ = -1;
    proc::reap(child_);
    if (on_exit) on_exit();
}

}  // namespace island
