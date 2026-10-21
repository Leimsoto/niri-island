#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace island {

// The single-threaded event loop. Everything that touches application state
// runs here; worker threads only compute and hand results back with post().
class Loop {
public:
    using FdCallback = std::function<void(short revents)>;
    using Task = std::function<void()>;

    Loop();
    ~Loop();

    static Loop& get();

    // fd sources
    void watch(int fd, short events, FdCallback cb);
    void unwatch(int fd);

    // Timers (seconds). Returns an id usable with cancel(). A repeat > 0 makes
    // it periodic.
    uint64_t after(double delay, Task fn, double repeat = 0.0);
    void cancel(uint64_t id);

    // Thread safe: runs fn on the loop thread soon.
    void post(Task fn);
    // From a thread that may outlive the loop (a detached download, PAM):
    // posts if the loop still exists, else drops the task.
    static void post_if_alive(Task fn);
    // Set once the program is on its way out: long transfers give up.
    static bool shutting_down();

    // Runs `work` on the worker pool, then `done` on the loop thread.
    void async(Task work, Task done = {});

    // Called before each poll; returns how long the loop may sleep (ms, -1 = forever).
    std::function<int()> before_poll;

    void run();
    void quit();
    bool running() const { return running_; }

private:
    struct Watch {
        int fd;
        short events;
        FdCallback cb;
        bool removed = false;
    };
    struct Timer {
        double due;
        double repeat;
        Task fn;
    };

    std::vector<Watch> watches_;
    std::map<uint64_t, Timer> timers_;
    uint64_t next_timer_ = 1;

    int wake_fd_ = -1;
    std::mutex post_mu_;
    std::vector<Task> posted_;

    // worker pool
    std::vector<std::thread> workers_;
    std::mutex work_mu_;
    std::condition_variable work_cv_;
    std::deque<Task> work_;
    bool stopping_ = false;

    bool running_ = true;

    void drain_posted();
    void run_timers();
    void worker_main();
};

}  // namespace island
