#pragma once

#include <cstdint>

#include <deque>
#include <string>
#include <vector>

namespace island {

struct SysStats {
    float cpu = 0;           // %
    float ram = 0;           // %
    float ram_used_gb = 0, ram_total_gb = 0;
    float swap = 0;          // %
    float temp = -1;         // °C of the CPU package
    float disk = 0;          // % of /
    float disk_free_gb = 0;
    float net_down = 0, net_up = 0;  // bytes/s
    float load1 = 0;
    float gpu = -1;          // % busy; -1: no way to read it (NVIDIA's closed driver)
    int gpu_mhz = 0, gpu_max_mhz = 0;
    std::string gpu_name;    // "Intel", "AMD"…
    int64_t uptime_s = 0;
    std::vector<float> cpu_hist, ram_hist, net_hist, gpu_hist;  // 0..1, oldest first
};

class SysMon {
public:
    SysMon();
    void sample();  // call about once a second while someone is looking
    const SysStats& stats() const { return st_; }

private:
    SysStats st_;
    uint64_t prev_total_ = 0, prev_idle_ = 0;
    uint64_t prev_rx_ = 0, prev_tx_ = 0;
    double prev_t_ = 0;
    std::string temp_path_;
    std::deque<float> cpu_h_, ram_h_, net_h_, gpu_h_;
    // GPU busy: Intel from the time its engines sleep (RC6 / gt idle
    // residency), AMD from its own gpu_busy_percent.
    enum class GpuKind { None, Idle, Busy } gpu_kind_ = GpuKind::None;
    std::string gpu_path_, gpu_freq_path_, gpu_max_path_;
    uint64_t prev_idle_ms_ = 0;
    void find_gpu();
    float net_peak_ = 64 * 1024;
    void find_temp();
};

std::string fmt_bytes_rate(float bps);

}  // namespace island
