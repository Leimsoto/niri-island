#include "sysmon.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace island {

SystemMonitor::SystemMonitor() {
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
            uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
            if (ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
                uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
                uint64_t idle_time = idle + iowait;

                if (prev_total_ > 0 && total > prev_total_) {
                    uint64_t total_delta = total - prev_total_;
                    uint64_t idle_delta = idle_time - prev_idle_;
                    stats_.cpu_percent = std::clamp(100.0f * (1.0f - static_cast<float>(idle_delta) / static_cast<float>(total_delta)), 0.0f, 100.0f);
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

    std::string line;
    uint64_t total_kb = 0;
    uint64_t avail_kb = 0;

    while (std::getline(file, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::istringstream ss(line.substr(9));
            ss >> total_kb;
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream ss(line.substr(13));
            ss >> avail_kb;
        }
        if (total_kb > 0 && avail_kb > 0) break;
    }

    if (total_kb > 0) {
        uint64_t used_kb = (total_kb > avail_kb) ? (total_kb - avail_kb) : 0;
        stats_.ram_total_gb = static_cast<float>(total_kb) / (1024.0f * 1024.0f);
        stats_.ram_used_gb = static_cast<float>(used_kb) / (1024.0f * 1024.0f);
        stats_.ram_percent = std::clamp(100.0f * static_cast<float>(used_kb) / static_cast<float>(total_kb), 0.0f, 100.0f);
    }
}

void SystemMonitor::update() {
    read_cpu();
    read_memory();
}

} // namespace island
