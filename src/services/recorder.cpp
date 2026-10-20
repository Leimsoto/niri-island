#include "services/recorder.hpp"

#include <csignal>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/config.hpp"
#include "core/loop.hpp"

namespace island {

using json = nlohmann::json;

static std::string options_path() { return state_dir() + "/recorder.json"; }

Recorder::~Recorder() {
    if (child_.pid > 0) {
        kill(child_.pid, SIGINT);
        proc::reap(child_);
    }
    teardown_audio();
}

void Recorder::init() {
    std::string text;
    if (!read_file(options_path(), text)) return;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    opts_.target = std::clamp(j.value("target", 0), 0, 1);
    opts_.audio = std::clamp(j.value("audio", 0), 0, 3);
    opts_.fps = j.value("fps", 60) == 30 ? 30 : 60;
    opts_.quality = std::clamp(j.value("quality", 1), 0, 2);
    opts_.encoder = std::clamp(j.value("encoder", 0), 0, 1);
    opts_.format = std::clamp(j.value("format", 0), 0, 2);
    opts_.countdown = std::clamp(j.value("countdown", 3), 0, 10);
}

void Recorder::save_options() {
    json j{{"target", opts_.target},   {"audio", opts_.audio},       {"fps", opts_.fps},
           {"quality", opts_.quality}, {"encoder", opts_.encoder},   {"format", opts_.format},
           {"countdown", opts_.countdown}};
    make_dirs(state_dir());
    write_file_atomic(options_path(), j.dump(2));
}

double Recorder::elapsed() const {
    double e = recorded_;
    if (state_ == State::Recording && seg_wall_start_ > 0) e += now_mono() - seg_wall_start_;
    return e;
}

int64_t Recorder::bytes() const {
    int64_t total = 0;
    for (auto& s : segments_) {
        struct stat st {};
        if (stat(s.c_str(), &st) == 0) total += st.st_size;
    }
    return total;
}

std::string Recorder::audio_device() const {
    switch (opts_.audio) {
        case 1: return sink_ + ".monitor";
        case 2: return source_;
        case 3: return "niri_island_mix.monitor";
        default: return "";
    }
}

void Recorder::setup_audio() {
    if (opts_.audio != 3) return;
    // System + microphone: a null sink both are looped into; record its monitor.
    auto load = [this](std::vector<std::string> args) {
        args.insert(args.begin(), {"pactl", "load-module"});
        auto r = proc::run(args, 4000);
        std::string id = trim(r.out);
        if (r.ok() && !id.empty()) modules_.push_back(id);
    };
    load({"module-null-sink", "sink_name=niri_island_mix", "sink_properties=device.description=niri-island-grabación"});
    load({"module-loopback", "source=" + source_, "sink=niri_island_mix", "latency_msec=30"});
    load({"module-loopback", "source=" + sink_ + ".monitor", "sink=niri_island_mix", "latency_msec=30"});
}

void Recorder::teardown_audio() {
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) proc::run({"pactl", "unload-module", *it}, 3000);
    modules_.clear();
}

std::vector<std::string> Recorder::command(const std::string& file) const {
    std::vector<std::string> a = {"wf-recorder", "-y"};
    if (!output_.empty()) a.insert(a.end(), {"-o", output_});
    if (region_.w > 0 && region_.h > 0) {
        char g[96];
        // Even sizes: H.264 needs them.
        int w = static_cast<int>(region_.w) & ~1, h = static_cast<int>(region_.h) & ~1;
        snprintf(g, sizeof(g), "%d,%d %dx%d", static_cast<int>(region_.x), static_cast<int>(region_.y), w, h);
        a.insert(a.end(), {"-g", g});
    }
    a.insert(a.end(), {"-r", std::to_string(opts_.fps)});
    bool gpu = opts_.encoder == 0 && !gpu_failed_ && file_exists("/dev/dri/renderD128");
    if (gpu) {
        static const char* qp[] = {"18", "24", "30"};
        a.insert(a.end(), {"-c", "h264_vaapi", "-d", "/dev/dri/renderD128", "-p", std::string("qp=") + qp[opts_.quality]});
    } else {
        static const char* crf[] = {"18", "23", "28"};
        a.insert(a.end(), {"-c", "libx264", "-p", "preset=veryfast", "-p", std::string("crf=") + crf[opts_.quality], "-x",
                           "yuv420p"});
    }
    std::string dev = audio_device();
    if (!dev.empty()) a.push_back("--audio=" + dev);
    a.insert(a.end(), {"-f", file});
    return a;
}

void Recorder::start(const Rect& region, const std::string& output, const std::string& sink, const std::string& source) {
    if (state_ != State::Idle) return;
    region_ = opts_.target == 1 ? region : Rect{};
    output_ = output;
    sink_ = sink;
    source_ = source;
    segments_.clear();
    recorded_ = 0;
    gpu_failed_ = false;
    stopping_ = discard_ = false;
    std::string base = config().recording_dir.empty() ? user_dir("VIDEOS", "Vídeos") + "/Grabaciones"
                                                      : expand_home(config().recording_dir);
    make_dirs(base);
    static const char* ext[] = {"mp4", "mkv", "gif"};
    final_ = base + "/" + fmt_time("Grabación_%Y-%m-%d_%H-%M-%S.") + ext[opts_.format];
    dir_ = cache_dir() + "/rec-" + std::to_string(now_epoch());
    make_dirs(dir_);
    log_ = dir_ + "/wf-recorder.log";
    countdown_left_ = opts_.countdown;
    if (countdown_left_ <= 0) {
        Loop::get().async([this] { setup_audio(); }, [this] {
            state_ = State::Recording;
            begin_segment();
            changed();
        });
        return;
    }
    state_ = State::Countdown;
    if (on_countdown) on_countdown(countdown_left_);
    changed();
    // The mix sink is ready before the countdown ends.
    Loop::get().async([this] { setup_audio(); });
    countdown_timer_ = Loop::get().after(1.0, [this] {
        if (state_ != State::Countdown) return;
        --countdown_left_;
        if (countdown_left_ > 0) {
            if (on_countdown) on_countdown(countdown_left_);
            changed();
            return;
        }
        Loop::get().cancel(countdown_timer_);
        countdown_timer_ = 0;
        state_ = State::Recording;
        begin_segment();
        changed();
    }, 1.0);
}

void Recorder::begin_segment() {
    char name[32];
    snprintf(name, sizeof(name), "/seg-%02zu.mkv", segments_.size());
    std::string file = dir_ + name;
    child_ = proc::start(command(file), log_);
    if (child_.pid <= 0) {
        state_ = State::Idle;
        if (on_done) on_done("", false, "No se pudo iniciar wf-recorder");
        return;
    }
    segments_.push_back(file);
    seg_started_ = seg_wall_start_ = now_mono();
    if (child_.pidfd >= 0) Loop::get().watch(child_.pidfd, POLLIN, [this](short) { on_exit(); });
}

void Recorder::end_segment() {
    if (child_.pid > 0) kill(child_.pid, SIGINT);  // wf-recorder finalises the file on SIGINT
}

void Recorder::on_exit() {
    int fd = child_.pidfd;
    if (fd >= 0) Loop::get().unwatch(fd);
    double lasted = now_mono() - seg_started_;
    int code = proc::reap(child_);
    if (state_ == State::Recording || state_ == State::Paused) recorded_ += now_mono() - seg_wall_start_;
    seg_wall_start_ = 0;

    bool unexpected = !stopping_ && state_ == State::Recording;
    if (unexpected) {
        // Died by itself. Right at the start with the GPU encoder: retry on the CPU.
        if (lasted < 3.0 && opts_.encoder == 0 && !gpu_failed_) {
            std::cerr << "[recorder] GPU encoder failed (" << code << "), retrying with x264\n";
            gpu_failed_ = true;
            if (!segments_.empty()) {
                unlink(segments_.back().c_str());
                segments_.pop_back();
            }
            recorded_ = 0;
            begin_segment();
            return;
        }
        std::string err;
        read_file(log_, err);
        if (err.size() > 300) err = err.substr(err.size() - 300);
        stopping_ = true;
        finalize();
        return;
    }
    if (stopping_) finalize();
    // Paused: nothing to do until resume().
}

void Recorder::pause() {
    if (state_ != State::Recording) return;
    state_ = State::Paused;
    end_segment();
    changed();
}

void Recorder::resume() {
    if (state_ != State::Paused) return;
    if (child_.pid > 0) {
        // The previous segment is still closing; try again in a moment.
        Loop::get().after(0.2, [this] { resume(); });
        return;
    }
    state_ = State::Recording;
    begin_segment();
    changed();
}

void Recorder::stop() {
    if (state_ == State::Countdown) {
        cancel();
        return;
    }
    if (state_ != State::Recording && state_ != State::Paused) return;
    stopping_ = true;
    bool running = child_.pid > 0;
    state_ = State::Saving;
    changed();
    if (running) end_segment();
    else finalize();
}

void Recorder::cancel() {
    if (state_ == State::Idle) return;
    Loop::get().cancel(countdown_timer_);
    countdown_timer_ = 0;
    discard_ = true;
    stopping_ = true;
    if (child_.pid > 0) {
        state_ = State::Saving;
        end_segment();
    } else {
        finalize();
    }
    changed();
}

void Recorder::finalize() {
    state_ = State::Saving;
    changed();
    auto segs = segments_;
    std::string dir = dir_, final = final_;
    bool discard = discard_;
    int format = opts_.format;
    auto ok = std::make_shared<bool>(false);
    auto err = std::make_shared<std::string>();
    Loop::get().async(
        [this, segs, dir, final, discard, format, ok, err] {
            teardown_audio();
            std::vector<std::string> good;
            for (auto& s : segs) {
                struct stat st {};
                if (stat(s.c_str(), &st) == 0 && st.st_size > 1024) good.push_back(s);
            }
            if (!discard && !good.empty()) {
                std::string joined = dir + "/joined.mkv";
                if (good.size() == 1) {
                    joined = good[0];
                } else {
                    std::string list;
                    for (auto& s : good) list += "file '" + replace_all(s, "'", "'\\''") + "'\n";
                    write_file_atomic(dir + "/list.txt", list);
                    proc::run({"ffmpeg", "-y", "-v", "error", "-f", "concat", "-safe", "0", "-i", dir + "/list.txt", "-c",
                               "copy", joined},
                              120000);
                }
                proc::Result r;
                if (format == 2) {
                    // GIF with its own palette, at a size that stays shareable.
                    r = proc::run({"ffmpeg", "-y", "-v", "error", "-i", joined, "-vf",
                                   "fps=15,scale='min(960,iw)':-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=4",
                                   "-loop", "0", final},
                                  300000, nullptr, true);
                } else if (format == 1) {
                    r = proc::run({"ffmpeg", "-y", "-v", "error", "-i", joined, "-c", "copy", final}, 120000, nullptr, true);
                } else {
                    r = proc::run({"ffmpeg", "-y", "-v", "error", "-i", joined, "-c", "copy", "-movflags", "+faststart", final},
                                  120000, nullptr, true);
                }
                *ok = r.ok() && file_exists(final);
                if (!*ok) *err = trim(r.out).substr(0, 300);
            } else if (!discard) {
                std::string log;
                read_file(dir + "/wf-recorder.log", log);
                *err = log.size() > 300 ? log.substr(log.size() - 300) : log;
            }
            // Clean the working directory.
            for (auto& s : segs) unlink(s.c_str());
            unlink((dir + "/joined.mkv").c_str());
            unlink((dir + "/list.txt").c_str());
            unlink((dir + "/wf-recorder.log").c_str());
            rmdir(dir.c_str());
        },
        [this, final, discard, ok, err] {
            state_ = State::Idle;
            segments_.clear();
            recorded_ = 0;
            stopping_ = false;
            changed();
            if (!discard && on_done) on_done(*ok ? final : "", *ok, *err);
        });
}

}  // namespace island
