#include "core/filewatch.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

FileWatch::~FileWatch() {
    if (fd_ >= 0) {
        Loop::get().unwatch(fd_);
        close(fd_);
    }
    for (auto* e : owned_) delete e;
}

void FileWatch::watch(const std::string& path, std::function<void()> cb) {
    if (path.empty()) return;
    if (fd_ < 0) {
        fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (fd_ < 0) return;
        Loop::get().watch(fd_, POLLIN, [this](short) { readable(); });
    }
    size_t slash = path.rfind('/');
    std::string dir = slash == std::string::npos ? "." : path.substr(0, slash);
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    make_dirs(dir);
    bool have = false;
    for (auto& [wd, d] : dirs_) have |= d == dir;
    if (!have) {
        int wd = inotify_add_watch(fd_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
        if (wd >= 0) dirs_[wd] = dir;
    }
    auto* e = new Entry{name, std::move(cb)};
    owned_.push_back(e);
    files_.emplace(dir, e);
}

void FileWatch::readable() {
    alignas(inotify_event) char buf[8192];
    for (;;) {
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) break;
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<inotify_event*>(p);
            p += sizeof(inotify_event) + ev->len;
            auto dit = dirs_.find(ev->wd);
            if (dit == dirs_.end() || ev->len == 0) continue;
            std::string name = ev->name;
            auto range = files_.equal_range(dit->second);
            for (auto it = range.first; it != range.second; ++it) {
                Entry* e = it->second;
                if (e->name != name) continue;
                // Writers often touch a file several times in a row.
                Loop::get().cancel(e->debounce);
                e->debounce = Loop::get().after(0.15, [e] {
                    e->debounce = 0;
                    e->cb();
                });
            }
        }
    }
}

}  // namespace island
