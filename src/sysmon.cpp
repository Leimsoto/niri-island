#include "sysmon.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace island {

SystemMonitor::SystemMonitor() {
    last_sample_time_ = std::chrono::steady_clock::now();
    first_sample_ = true;
    read_cpu();
    read_memory();
}

void SystemMonitor::read_cpu() {
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return;

    std::string line;
    if (std::getline(file, line)) {
        if (line.rfind("cpu ", 0) == 0) {
            std::istringstream ss(line.substr(4));
            uint64_t user = 0, nice = 0, system = 0, idle = 0;
            uint64_t iowait = 0, irq = 0, softirq = 0, steal = 0;
            if (ss >> user >> nice >> system >> idle) {
                ss >> iowait >> irq >> softirq >> steal;
                uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
                uint64_t idle_time = idle + iowait;

                if (prev_total_ > 0 && total > prev_total_) {
                    uint64_t total_delta = total - prev_total_;
                    uint64_t idle_delta = (idle_time >= prev_idle_) ? (idle_time - prev_idle_) : 0;
                    float raw_usage = std::clamp(100.0f * (1.0f - static_cast<float>(idle_delta) / static_cast<float>(total_delta)), 0.0f, 100.0f);
                    
                    if (stats_.cpu_percent <= 0.01f) {
                        stats_.cpu_percent = raw_usage;
                    } else {
                        // Smooth with EMA: 70% current sample, 30% previous for stable readings
                        stats_.cpu_percent = stats_.cpu_percent * 0.30f + raw_usage * 0.70f;
                    }
                }

                prev_total_ = total;
                prev_idle_ = idle_time;
            }
        }
    }
}

void SystemMonitor::read_memory() {
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return;

    std::string key, unit;
    uint64_t value_kb = 0;
    uint64_t total_kb = 0;
    uint64_t avail_kb = 0;

    while (file >> key >> value_kb >> unit) {
        if (key == "MemTotal:") {
            total_kb = value_kb;
        } else if (key == "MemAvailable:") {
            avail_kb = value_kb;
        }
        if (total_kb > 0 && avail_kb > 0) break;
    }

    if (total_kb > 0) {
        uint64_t used_kb = (total_kb > avail_kb) ? (total_kb - avail_kb) : 0;
        stats_.ram_total_gb = static_cast<float>(total_kb) / (1024.0f * 1024.0f);
        stats_.ram_used_gb = static_cast<float>(used_kb) / (1024.0f * 1024.0f);
        float raw_ram = std::clamp(100.0f * static_cast<float>(used_kb) / static_cast<float>(total_kb), 0.0f, 100.0f);
        if (stats_.ram_percent <= 0.01f) {
            stats_.ram_percent = raw_ram;
        } else {
            stats_.ram_percent = stats_.ram_percent * 0.35f + raw_ram * 0.65f;
        }
    }
}

void SystemMonitor::update() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - last_sample_time_).count();
    // Sample on a fixed 1-second cadence (Noctalia standard) to avoid micro-jitter
    if (elapsed < 1.0f && !first_sample_) {
        return;
    }
    last_sample_time_ = now;
    first_sample_ = false;

    read_cpu();
    read_memory();
}

} // namespace island
