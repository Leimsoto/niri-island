#include "core/loop.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "core/util.hpp"

namespace island {

static Loop* g_loop = nullptr;

Loop& Loop::get() { return *g_loop; }

Loop::Loop() {
    g_loop = this;
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    watch(wake_fd_, POLLIN, [this](short) {
        uint64_t v;
        while (read(wake_fd_, &v, sizeof(v)) > 0) {}
        drain_posted();
    });
    unsigned n = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { worker_main(); });
}

Loop::~Loop() {
    {
        std::lock_guard<std::mutex> lock(work_mu_);
        stopping_ = true;
        work_.clear();
    }
    work_cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    if (wake_fd_ >= 0) close(wake_fd_);
    if (g_loop == this) g_loop = nullptr;
}

void Loop::watch(int fd, short events, FdCallback cb) {
    if (fd < 0) return;
    for (auto& w : watches_) {
        if (w.fd == fd && !w.removed) {
            w.events = events;
            w.cb = std::move(cb);
            return;
        }
    }
    watches_.push_back({fd, events, std::move(cb)});
}

void Loop::unwatch(int fd) {
    for (auto& w : watches_)
        if (w.fd == fd) w.removed = true;
}

uint64_t Loop::after(double delay, Task fn, double repeat) {
    uint64_t id = next_timer_++;
    timers_[id] = {now_mono() + std::max(0.0, delay), repeat, std::move(fn)};
    return id;
}

void Loop::cancel(uint64_t id) {
    if (id) timers_.erase(id);
}

void Loop::post(Task fn) {
    {
        std::lock_guard<std::mutex> lock(post_mu_);
        posted_.push_back(std::move(fn));
    }
    uint64_t one = 1;
    ssize_t r = write(wake_fd_, &one, sizeof(one));
    (void)r;
}

void Loop::async(Task work, Task done) {
    auto job = [this, work = std::move(work), done = std::move(done)]() mutable {
        try {
            if (work) work();
        } catch (const std::exception& e) {
            std::cerr << "[worker] exception: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[worker] unknown exception\n";
        }
        if (done) post(std::move(done));
    };
    {
        std::lock_guard<std::mutex> lock(work_mu_);
        work_.push_back(std::move(job));
    }
    work_cv_.notify_one();
}

void Loop::worker_main() {
    for (;;) {
        Task job;
        {
            std::unique_lock<std::mutex> lock(work_mu_);
            work_cv_.wait(lock, [this] { return stopping_ || !work_.empty(); });
            if (stopping_) return;
            job = std::move(work_.front());
            work_.pop_front();
        }
        job();
    }
}

void Loop::drain_posted() {
    std::vector<Task> tasks;
    {
        std::lock_guard<std::mutex> lock(post_mu_);
        tasks.swap(posted_);
    }
    for (auto& t : tasks) {
        try {
            t();
        } catch (const std::exception& e) {
            std::cerr << "[loop] task exception: " << e.what() << "\n";
        }
    }
}

void Loop::run_timers() {
    double now = now_mono();
    std::vector<uint64_t> due;
    for (auto& [id, t] : timers_)
        if (t.due <= now) due.push_back(id);
    for (uint64_t id : due) {
        auto it = timers_.find(id);
        if (it == timers_.end()) continue;  // cancelled by an earlier timer
        Task fn = it->second.fn;
        if (it->second.repeat > 0) {
            it->second.due = now + it->second.repeat;
        } else {
            timers_.erase(it);
        }
        fn();
    }
}

void Loop::run() {
    std::vector<pollfd> pfds;
    while (running_) {
        int timeout = before_poll ? before_poll() : -1;
        if (!running_) break;
        if (!timers_.empty()) {
            double next = 1e18;
            for (auto& [id, t] : timers_) next = std::min(next, t.due);
            int ms = static_cast<int>(std::ceil(std::max(0.0, next - now_mono()) * 1000.0));
            timeout = (timeout < 0) ? ms : std::min(timeout, ms);
        }

        watches_.erase(std::remove_if(watches_.begin(), watches_.end(), [](const Watch& w) { return w.removed; }),
                       watches_.end());
        pfds.clear();
        for (auto& w : watches_) pfds.push_back({w.fd, w.events, 0});

        int r = poll(pfds.data(), pfds.size(), timeout);
        if (r < 0 && errno != EINTR) {
            std::cerr << "[loop] poll failed: " << strerror(errno) << "\n";
            break;
        }
        if (r > 0) {
            // Callbacks may add or remove watches; iterate over a snapshot.
            size_t count = pfds.size();
            for (size_t i = 0; i < count && running_; ++i) {
                if (!pfds[i].revents) continue;
                int fd = pfds[i].fd;
                for (auto& w : watches_) {
                    if (w.fd == fd && !w.removed) {
                        auto cb = w.cb;
                        cb(pfds[i].revents);
                        break;
                    }
                }
            }
        }
        run_timers();
    }
}

}  // namespace island
