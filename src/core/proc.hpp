#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace island::proc {

struct Result {
    int status = -1;    // exit code, or -1 when it could not run / was killed
    std::string out;    // stdout (stderr too when merge_stderr)
    bool ok() const { return status == 0; }
};

// Runs a program directly (no shell, so arguments are never interpreted),
// waiting at most `timeout_ms`. Blocking: call it from a worker, never from
// the main loop.
Result run(const std::vector<std::string>& argv, int timeout_ms = 10000, const std::string* input = nullptr,
           bool merge_stderr = false);

// Like run(), but output goes to /dev/null: for programs that keep running
// in the background holding stdout (wl-copy serving the clipboard).
int run_quiet(const std::vector<std::string>& argv, const std::string* input = nullptr, int timeout_ms = 8000);

// Starts a supervised child (stdout/stderr appended to `log`). Returns its
// pid and a pidfd that becomes readable when it exits (-1 on failure).
struct Child {
    int pid = -1;
    int pidfd = -1;
};
Child start(const std::vector<std::string>& argv, const std::string& log);
// Like start(), but its stdin and stdout are pipes back to us (`in` to write
// to it, `out` to read from it; both non-blocking, close-on-exec).
Child start_piped(const std::vector<std::string>& argv, int& in, int& out, const std::string& log);
// Reaps a supervised child; returns its exit status (or -1).
int reap(Child& c);

// Starts a program and forgets it (double fork, own session, no zombies).
void spawn(const std::vector<std::string>& argv);

// For user-configured commands only: runs `sh -c cmd`, detached.
void spawn_shell(const std::string& cmd);

// Whether `name` resolves in PATH (cached).
bool has(const std::string& name);

// Set on shutdown: running children are killed instead of awaited.
extern std::atomic<bool> g_shutdown;

}  // namespace island::proc
