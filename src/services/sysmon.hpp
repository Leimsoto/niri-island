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
    int64_t uptime_s = 0;
    std::vector<float> cpu_hist, ram_hist, net_hist;  // 0..1, oldest first
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
    std::deque<float> cpu_h_, ram_h_, net_h_;
    float net_peak_ = 64 * 1024;
    void find_temp();
};

std::string fmt_bytes_rate(float bps);

}  // namespace island
