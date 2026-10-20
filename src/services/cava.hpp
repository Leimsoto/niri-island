#pragma once

#include <cstdint>

#include <string>
#include <sys/types.h>
#include <vector>

namespace island {

// A live audio spectrum from cava (raw ascii output), run only while
// something is playing and the island shows it.
class Cava {
public:
    ~Cava();
    bool available() const;
    void set_running(bool on);
    bool running() const { return pid_ > 0; }
    const std::vector<float>& bars() const { return bars_; }
    // True while real, non-silent data is arriving.
    bool live() const;

    static constexpr int kBars = 12;

private:
    pid_t pid_ = -1;
    int fd_ = -1;
    std::string buf_;
    std::vector<float> bars_ = std::vector<float>(kBars, 0.0f);
    bool live_ = false;
    double last_signal_ = 0;
    void start();
    void stop();
    void readable();
};

}  // namespace island
