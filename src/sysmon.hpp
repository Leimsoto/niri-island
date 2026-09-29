#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace island {

struct SystemStats {
    float cpu_percent = 0.0f;
    float ram_percent = 0.0f;
    float ram_used_gb = 0.0f;
    float ram_total_gb = 0.0f;
};

class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor() = default;

    void update();
    const SystemStats& stats() const { return stats_; }

private:
    SystemStats stats_;
    uint64_t prev_total_ = 0;
    uint64_t prev_idle_ = 0;
    std::chrono::steady_clock::time_point last_sample_time_{};
    bool first_sample_ = true;

    void read_cpu();
    void read_memory();
};

} // namespace island
