#include "services/timer.hpp"

#include <cctype>
#include <cmath>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

double Timer::remaining() const {
    if (!running_) return 0;
    if (paused_) return remaining_at_pause_;
    return std::max(0.0, ends_at_ - now_mono());
}

float Timer::progress() const {
    if (!running_ || duration_ <= 0) return 0;
    return static_cast<float>(std::clamp(remaining() / duration_, 0.0, 1.0));
}

void Timer::arm() {
    Loop::get().cancel(timer_);
    timer_ = 0;
    if (!running_ || paused_) return;
    timer_ = Loop::get().after(remaining(), [this] {
        timer_ = 0;
        std::string l = label_;
        running_ = false;
        paused_ = false;
        label_.clear();
        if (on_finish) on_finish(l);
        if (on_change) on_change();
    });
}

void Timer::start(double seconds, const std::string& label) {
    if (seconds <= 0) return;
    duration_ = seconds;
    ends_at_ = now_mono() + seconds;
    label_ = label;
    running_ = true;
    paused_ = false;
    arm();
    if (on_change) on_change();
}

void Timer::pause() {
    if (!running_ || paused_) return;
    remaining_at_pause_ = remaining();
    paused_ = true;
    arm();
    if (on_change) on_change();
}

void Timer::resume() {
    if (!running_ || !paused_) return;
    ends_at_ = now_mono() + remaining_at_pause_;
    paused_ = false;
    arm();
    if (on_change) on_change();
}

void Timer::add(double seconds) {
    if (!running_) {
        start(seconds, label_);
        return;
    }
    if (paused_) remaining_at_pause_ += seconds;
    else ends_at_ += seconds;
    duration_ += seconds;
    arm();
    if (on_change) on_change();
}

void Timer::cancel() {
    running_ = paused_ = false;
    label_.clear();
    arm();
    if (on_change) on_change();
}

double Timer::parse(const std::string& input, std::string* rest) {
    std::string s = trim(input);
    // Split off a label: the duration is the first word.
    std::string head = s, tail;
    size_t sp = s.find(' ');
    if (sp != std::string::npos) {
        head = s.substr(0, sp);
        tail = trim(s.substr(sp + 1));
    }
    head = lower(head);
    if (rest) *rest = tail;
    if (head.empty()) return 0;
    int a = 0, b = 0, c = 0;
    char extra;
    if (sscanf(head.c_str(), "%d:%d:%d%c", &a, &b, &c, &extra) == 3) return a * 3600 + b * 60 + c;
    if (sscanf(head.c_str(), "%d:%d%c", &a, &b, &extra) == 2) return a * 60 + b;
    double total = 0;
    char last = 0;
    size_t i = 0;
    bool any = false;
    while (i < head.size()) {
        size_t start = i;
        while (i < head.size() && (std::isdigit(static_cast<unsigned char>(head[i])) || head[i] == '.')) i++;
        if (start == i) return 0;
        double num = std::atof(head.substr(start, i - start).c_str());
        char unit = i < head.size() ? head[i] : 0;
        if (unit == 'h' || unit == 'm' || unit == 's') {
            i++;
        } else if (unit == 0) {
            unit = last == 'h' ? 'm' : last == 'm' ? 's' : 'm';
        } else {
            return 0;
        }
        total += num * (unit == 'h' ? 3600 : unit == 'm' ? 60 : 1);
        last = unit;
        any = true;
    }
    return any ? std::round(total) : 0;
}

double Stopwatch::elapsed() const {
    if (!running_) return 0;
    return banked_ + (paused_ ? 0 : now_mono() - started_);
}

void Stopwatch::start() {
    running_ = true;
    paused_ = false;
    banked_ = 0;
    started_ = now_mono();
    laps_.clear();
    if (on_change) on_change();
}

void Stopwatch::pause() {
    if (!running_ || paused_) return;
    banked_ += now_mono() - started_;
    paused_ = true;
    if (on_change) on_change();
}

void Stopwatch::resume() {
    if (!running_ || !paused_) return;
    started_ = now_mono();
    paused_ = false;
    if (on_change) on_change();
}

void Stopwatch::toggle() {
    if (!running_) start();
    else if (paused_) resume();
    else pause();
}

void Stopwatch::lap() {
    if (!running_ || paused_) return;
    laps_.push_back(elapsed());
    if (on_change) on_change();
}

void Stopwatch::reset() {
    running_ = paused_ = false;
    banked_ = 0;
    laps_.clear();
    if (on_change) on_change();
}

}  // namespace island
