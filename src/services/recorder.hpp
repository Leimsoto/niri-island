#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/proc.hpp"
#include "core/util.hpp"

namespace island {

struct RecOptions {
    int target = 0;    // 0 screen, 1 region
    int audio = 0;     // 0 none, 1 system, 2 microphone, 3 both
    int fps = 60;      // 30 | 60
    int quality = 1;   // 0 high, 1 balanced, 2 small
    int encoder = 0;   // 0 GPU (VA-API), 1 CPU (x264)
    int format = 0;    // 0 MP4, 1 MKV, 2 GIF
    int countdown = 3; // seconds
};

// Screen recording on wf-recorder, with what wf-recorder lacks: pause and
// resume (as segments joined by ffmpeg), system + microphone audio mixed
// through a temporary null sink, GPU encoding with automatic CPU fallback,
// and GIF export.
class Recorder {
public:
    enum class State { Idle, Countdown, Recording, Paused, Saving };

    ~Recorder();
    void init();

    State state() const { return state_; }
    bool active() const { return state_ != State::Idle; }
    RecOptions& options() { return opts_; }
    void save_options();
    double elapsed() const;
    int countdown() const { return countdown_left_; }
    int64_t bytes() const;
    const std::string& output_path() const { return final_; }
    bool has_region() const { return region_.w > 0; }

    // Begins the countdown, then records. `output` is the niri output name.
    void start(const Rect& region, const std::string& output, const std::string& sink, const std::string& source);
    void pause();
    void resume();
    void toggle_pause() { state_ == State::Paused ? resume() : pause(); }
    void stop();     // finish and save
    void cancel();   // discard

    std::function<void()> on_change;
    std::function<void(int left)> on_countdown;
    std::function<void(const std::string& path, bool ok, const std::string& error)> on_done;

private:
    State state_ = State::Idle;
    RecOptions opts_;
    Rect region_;
    std::string output_, sink_, source_;
    std::string dir_, final_, log_;
    std::vector<std::string> segments_;
    proc::Child child_;
    double seg_started_ = 0, recorded_ = 0, seg_wall_start_ = 0;
    int countdown_left_ = 0;
    uint64_t countdown_timer_ = 0;
    std::vector<std::string> modules_;  // pactl modules to unload
    bool gpu_failed_ = false;
    bool stopping_ = false, discard_ = false;

    void begin_segment();
    void end_segment();
    void on_exit();
    void finalize();
    void setup_audio();
    void teardown_audio();
    std::string audio_device() const;
    std::vector<std::string> command(const std::string& file) const;
    void changed() {
        if (on_change) on_change();
    }
};

}  // namespace island
