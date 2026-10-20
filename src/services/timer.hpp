#pragma once

#include <cstdint>

#include <functional>
#include <string>
#include <vector>

namespace island {

// One countdown, owned by the island. The end is a wall-clock instant, so a
// busy machine never makes it drift.
class Timer {
public:
    bool running() const { return running_; }
    bool paused() const { return paused_; }
    const std::string& label() const { return label_; }
    double remaining() const;      // seconds
    double duration() const { return duration_; }
    float progress() const;        // 1 → 0

    void start(double seconds, const std::string& label = "");
    void pause();
    void resume();
    void toggle() { paused_ ? resume() : pause(); }
    void add(double seconds);
    void cancel();
    void restart() {
        if (duration_ > 0) start(duration_, label_);
    }

    // "25m", "1h30", "90s", "5" (minutes), "1:30". Returns seconds or 0.
    static double parse(const std::string& text, std::string* rest = nullptr);

    std::function<void(const std::string& label)> on_finish;
    std::function<void()> on_change;

private:
    bool running_ = false, paused_ = false;
    std::string label_;
    double ends_at_ = 0, duration_ = 0, remaining_at_pause_ = 0;
    uint64_t timer_ = 0;
    void arm();
};

// A stopwatch with laps. Like the timer, it is measured on the monotonic clock.
class Stopwatch {
public:
    bool running() const { return running_; }   // started (possibly paused)
    bool paused() const { return paused_; }
    double elapsed() const;
    const std::vector<double>& laps() const { return laps_; }  // lap end times, oldest first

    void start();
    void pause();
    void resume();
    void toggle();
    void lap();
    void reset();

    std::function<void()> on_change;

private:
    bool running_ = false, paused_ = false;
    double started_ = 0, banked_ = 0;
    std::vector<double> laps_;
};

}  // namespace island
