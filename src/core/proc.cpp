#include "core/proc.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#include "core/util.hpp"

namespace island::proc {

std::atomic<bool> g_shutdown{false};

static std::vector<char*> make_argv(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    out.reserve(argv.size() + 1);
    for (auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

static void reset_child_signals() {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, nullptr);
    signal(SIGPIPE, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
}

Result run(const std::vector<std::string>& argv, int timeout_ms, const std::string* input, bool merge_stderr) {
    Result res;
    if (argv.empty()) return res;

    int out_pipe[2];
    int in_pipe[2] = {-1, -1};
    if (pipe2(out_pipe, O_CLOEXEC) < 0) return res;
    if (input && pipe2(in_pipe, O_CLOEXEC) < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return res;
    }

    auto args = make_argv(argv);
    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (input) {
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        return res;
    }
    if (pid == 0) {
        reset_child_signals();
        int devnull = open("/dev/null", O_RDWR);
        dup2(input ? in_pipe[0] : devnull, STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(merge_stderr ? out_pipe[1] : devnull, STDERR_FILENO);
        setenv("LC_ALL", "C.UTF-8", 1);  // stable, parseable output that keeps UTF-8 names
        execvp(args[0], args.data());
        _exit(127);
    }

    close(out_pipe[1]);
    if (input) {
        close(in_pipe[0]);
        const char* p = input->data();
        size_t left = input->size();
        while (left > 0) {
            ssize_t w = write(in_pipe[1], p, left);
            if (w <= 0) break;
            p += w;
            left -= static_cast<size_t>(w);
        }
        close(in_pipe[1]);
    }

    double deadline = now_mono() + timeout_ms / 1000.0;
    bool killed = false;
    char buf[4096];
    for (;;) {
        if (g_shutdown.load() || now_mono() > deadline) {
            kill(pid, SIGKILL);
            killed = true;
            break;
        }
        struct pollfd pfd {out_pipe[0], POLLIN, 0};
        int r = poll(&pfd, 1, 100);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                res.out.append(buf, static_cast<size_t>(n));
                if (res.out.size() > (8u << 20)) {  // runaway output guard
                    kill(pid, SIGKILL);
                    killed = true;
                    break;
                }
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                break;
            }
        }
    }
    close(out_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!killed && WIFEXITED(status)) res.status = WEXITSTATUS(status);
    return res;
}

int run_quiet(const std::vector<std::string>& argv, const std::string* input, int timeout_ms) {
    if (argv.empty()) return -1;
    int in_pipe[2] = {-1, -1};
    if (input && pipe2(in_pipe, O_CLOEXEC) < 0) return -1;
    auto args = make_argv(argv);
    pid_t pid = fork();
    if (pid < 0) {
        if (input) {
            close(in_pipe[0]);
            close(in_pipe[1]);
        }
        return -1;
    }
    if (pid == 0) {
        reset_child_signals();
        int devnull = open("/dev/null", O_RDWR);
        dup2(input ? in_pipe[0] : devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        execvp(args[0], args.data());
        _exit(127);
    }
    if (input) {
        close(in_pipe[0]);
        const char* p = input->data();
        size_t left = input->size();
        while (left > 0) {
            ssize_t w = write(in_pipe[1], p, left);
            if (w <= 0) break;
            p += w;
            left -= static_cast<size_t>(w);
        }
        close(in_pipe[1]);
    }
    double deadline = now_mono() + timeout_ms / 1000.0;
    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0 && errno != EINTR) return -1;
        if (g_shutdown.load() || now_mono() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        usleep(5000);
    }
}

Child start(const std::vector<std::string>& argv, const std::string& log) {
    Child c;
    if (argv.empty()) return c;
    auto args = make_argv(argv);
    pid_t pid = fork();
    if (pid < 0) return c;
    if (pid == 0) {
        reset_child_signals();
        setpgid(0, 0);
        int devnull = open("/dev/null", O_RDWR);
        int lf = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        dup2(devnull, STDIN_FILENO);
        dup2(lf >= 0 ? lf : devnull, STDOUT_FILENO);
        dup2(lf >= 0 ? lf : devnull, STDERR_FILENO);
        execvp(args[0], args.data());
        _exit(127);
    }
    c.pid = pid;
    c.pidfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    return c;
}

int reap(Child& c) {
    int status = 0, code = -1;
    if (c.pid > 0 && waitpid(c.pid, &status, 0) == c.pid && WIFEXITED(status)) code = WEXITSTATUS(status);
    if (c.pidfd >= 0) close(c.pidfd);
    c = Child{};
    return code;
}

void spawn(const std::vector<std::string>& argv) {
    if (argv.empty()) return;
    auto args = make_argv(argv);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        reset_child_signals();
        setsid();
        pid_t inner = fork();
        if (inner != 0) _exit(0);
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        for (int fd = 3; fd < 1024; ++fd) close(fd);
        chdir(home_dir().c_str());
        execvp(args[0], args.data());
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
}

void spawn_shell(const std::string& cmd) {
    if (trim(cmd).empty()) return;
    spawn({"/bin/sh", "-c", cmd});
}

bool has(const std::string& name) {
    static std::mutex mu;
    static std::unordered_map<std::string, bool> cache;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    bool found = false;
    const char* path = getenv("PATH");
    if (path) {
        for (auto& dir : split(path, ':')) {
            if (dir.empty()) continue;
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) {
                found = true;
                break;
            }
        }
    }
    cache[name] = found;
    return found;
}

}  // namespace island::proc
