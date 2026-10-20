#include "services/cava.hpp"

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"

namespace island {

Cava::~Cava() { stop(); }

bool Cava::live() const {
    // Silence (audio routed elsewhere, a muted player) falls back to the
    // synthetic animation instead of flat bars.
    return live_ && now_mono() - last_signal_ < 1.5;
}

bool Cava::available() const { return proc::has("cava"); }

void Cava::set_running(bool on) {
    if (on && pid_ <= 0 && available()) start();
    else if (!on && pid_ > 0) stop();
}

void Cava::start() {
    std::string dir = cache_dir();
    make_dirs(dir);
    std::string cfg = dir + "/cava.conf";
    char conf[512];
    snprintf(conf, sizeof(conf),
             "[general]\nbars = %d\nframerate = 40\nsensitivity = 110\n"
             "[input]\nmethod = pulse\nsource = auto\n"
             "[output]\nmethod = raw\nraw_target = /dev/stdout\ndata_format = ascii\nascii_max_range = 1000\n"
             "bar_delimiter = 59\nframe_delimiter = 10\nchannels = mono\n"
             "[smoothing]\nnoise_reduction = 66\n",
             kBars);
    write_file_atomic(cfg, conf);

    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return;
    }
    if (pid == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, nullptr);
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(p[1], STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        execlp("cava", "cava", "-p", cfg.c_str(), nullptr);
        _exit(127);
    }
    close(p[1]);
    fd_ = p[0];
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) | O_NONBLOCK);
    pid_ = pid;
    live_ = false;
    buf_.clear();
    Loop::get().watch(fd_, POLLIN, [this](short ev) {
        readable();
        if ((ev & (POLLHUP | POLLERR)) && fd_ >= 0) stop();
    });
}

void Cava::stop() {
    if (fd_ >= 0) {
        Loop::get().unwatch(fd_);
        close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        pid_t child = pid_;
        // Reap without blocking the loop.
        Loop::get().after(0.5, [child] {
            int st;
            if (waitpid(child, &st, WNOHANG) == 0) {
                kill(child, SIGKILL);
                waitpid(child, &st, 0);
            }
        });
        pid_ = -1;
    }
    live_ = false;
    std::fill(bars_.begin(), bars_.end(), 0.0f);
}

void Cava::readable() {
    char tmp[4096];
    ssize_t n;
    while ((n = read(fd_, tmp, sizeof(tmp))) > 0) buf_.append(tmp, static_cast<size_t>(n));
    // Keep only the newest complete frame.
    size_t last = buf_.rfind('\n');
    if (last == std::string::npos) return;
    size_t prev = buf_.rfind('\n', last == 0 ? 0 : last - 1);
    std::string frame = buf_.substr(prev == std::string::npos ? 0 : prev + 1,
                                    last - (prev == std::string::npos ? 0 : prev + 1));
    buf_.erase(0, last + 1);
    auto parts = split(frame, ';');
    int i = 0;
    float peak = 0;
    for (auto& p : parts) {
        if (p.empty() || i >= kBars) continue;
        bars_[i] = clamp01(std::atoi(p.c_str()) / 1000.0f);
        peak = std::max(peak, bars_[i]);
        ++i;
    }
    live_ = true;
    if (peak > 0.03f) last_signal_ = now_mono();
}

}  // namespace island
