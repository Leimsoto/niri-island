#include "services/sysmon.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sstream>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

#include "core/util.hpp"

namespace island {

std::string fmt_bytes_rate(float bps) {
    char buf[32];
    if (bps >= 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB/s", bps / (1024 * 1024));
    else if (bps >= 1024) snprintf(buf, sizeof(buf), "%.0f KB/s", bps / 1024);
    else snprintf(buf, sizeof(buf), "%.0f B/s", bps);
    return buf;
}

SysMon::SysMon() {
    find_temp();
    find_gpu();
    sample();
}

void SysMon::find_temp() {
    // hwmon chips that report the CPU package, in order of preference.
    const char* wanted[] = {"k10temp", "coretemp", "zenpower", "cpu_thermal", "acpitz"};
    DIR* d = opendir("/sys/class/hwmon");
    if (!d) return;
    std::vector<std::pair<int, std::string>> found;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string base = std::string("/sys/class/hwmon/") + e->d_name;
        std::string name;
        read_file(base + "/name", name);
        name = trim(name);
        for (int i = 0; i < 5; ++i)
            if (name == wanted[i]) found.emplace_back(i, base + "/temp1_input");
    }
    closedir(d);
    std::sort(found.begin(), found.end());
    if (!found.empty()) temp_path_ = found.front().second;
}

void SysMon::find_gpu() {
    DIR* d = opendir("/sys/class/drm");
    if (!d) return;
    std::vector<std::string> cards;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (starts_with(n, "card") && n.find('-') == std::string::npos) cards.push_back(n);
    }
    closedir(d);
    std::sort(cards.begin(), cards.end());
    for (auto& card : cards) {
        std::string base = "/sys/class/drm/" + card, vendor;
        read_file(base + "/device/vendor", vendor);
        vendor = trim(vendor);
        if (vendor == "0x1002" && file_exists(base + "/device/gpu_busy_percent")) {
            gpu_kind_ = GpuKind::Busy;
            gpu_path_ = base + "/device/gpu_busy_percent";
            st_.gpu_name = "AMD";
            return;
        }
        if (vendor == "0x8086") {
            // i915, then xe
            for (std::string idle : {base + "/gt/gt0/rc6_residency_ms", base + "/power/rc6_residency_ms",
                                     base + "/device/tile0/gt0/gtidle/idle_residency_ms"}) {
                if (!file_exists(idle)) continue;
                gpu_kind_ = GpuKind::Idle;
                gpu_path_ = idle;
                st_.gpu_name = "Intel";
                for (std::string f : {base + "/gt/gt0/rps_act_freq_mhz", base + "/gt_act_freq_mhz",
                                      base + "/device/tile0/gt0/freq0/act_freq"})
                    if (file_exists(f)) { gpu_freq_path_ = f; break; }
                for (std::string f : {base + "/gt/gt0/rps_max_freq_mhz", base + "/gt_max_freq_mhz",
                                      base + "/device/tile0/gt0/freq0/max_freq"})
                    if (file_exists(f)) { gpu_max_path_ = f; break; }
                return;
            }
        }
    }
}

static void push(std::deque<float>& h, float v) {
    h.push_back(clamp01(v));
    while (h.size() > 48) h.pop_front();
}

void SysMon::sample() {
    double now = now_mono();
    double dt = prev_t_ > 0 ? now - prev_t_ : 1.0;
    prev_t_ = now;

    std::string stat;
    if (read_file("/proc/stat", stat)) {
        std::istringstream ss(stat);
        std::string cpu;
        uint64_t v[10] = {0};
        ss >> cpu;
        for (auto& x : v) ss >> x;
        uint64_t idle = v[3] + v[4];
        uint64_t total = 0;
        for (int i = 0; i < 8; ++i) total += v[i];
        if (prev_total_ && total > prev_total_) {
            float usage = 100.0f * (1.0f - float(idle - prev_idle_) / float(total - prev_total_));
            st_.cpu = std::clamp(st_.cpu * 0.35f + usage * 0.65f, 0.0f, 100.0f);
        }
        prev_total_ = total;
        prev_idle_ = idle;
    }

    std::string mem;
    if (read_file("/proc/meminfo", mem)) {
        uint64_t total = 0, avail = 0, swt = 0, swf = 0;
        for (auto& line : split(mem, '\n')) {
            unsigned long long val = 0;
            char key[64];
            if (sscanf(line.c_str(), "%63[^:]: %llu", key, &val) != 2) continue;
            std::string k = key;
            if (k == "MemTotal") total = val;
            else if (k == "MemAvailable") avail = val;
            else if (k == "SwapTotal") swt = val;
            else if (k == "SwapFree") swf = val;
        }
        if (total) {
            st_.ram_total_gb = total / 1048576.0f;
            st_.ram_used_gb = (total - avail) / 1048576.0f;
            st_.ram = 100.0f * (total - avail) / total;
        }
        st_.swap = swt ? 100.0f * (swt - swf) / swt : 0;
    }

    if (!temp_path_.empty()) {
        std::string t;
        if (read_file(temp_path_, t)) st_.temp = std::atof(t.c_str()) / 1000.0f;
    }

    struct statvfs vfs {};
    if (statvfs("/", &vfs) == 0 && vfs.f_blocks) {
        st_.disk = 100.0f * (1.0f - float(vfs.f_bavail) / float(vfs.f_blocks));
        st_.disk_free_gb = float(vfs.f_bavail) * vfs.f_frsize / 1073741824.0f;
    }

    std::string net;
    if (read_file("/proc/net/dev", net)) {
        uint64_t rx = 0, tx = 0;
        for (auto& line : split(net, '\n')) {
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string ifc = trim(line.substr(0, c));
            if (ifc == "lo" || starts_with(ifc, "veth") || starts_with(ifc, "docker") || starts_with(ifc, "br-") ||
                starts_with(ifc, "virbr"))
                continue;
            std::istringstream ss(line.substr(c + 1));
            uint64_t f[16] = {0};
            for (auto& x : f) ss >> x;
            rx += f[0];
            tx += f[8];
        }
        if (prev_rx_ && dt > 0.05) {
            st_.net_down = static_cast<float>((rx - prev_rx_) / dt);
            st_.net_up = static_cast<float>((tx - prev_tx_) / dt);
        }
        prev_rx_ = rx;
        prev_tx_ = tx;
    }

    if (gpu_kind_ != GpuKind::None) {
        std::string t;
        if (read_file(gpu_path_, t)) {
            float busy = -1;
            if (gpu_kind_ == GpuKind::Busy) {
                busy = static_cast<float>(std::atof(t.c_str()));
            } else {
                uint64_t idle = std::strtoull(t.c_str(), nullptr, 10);
                if (prev_idle_ms_ && dt > 0.05)
                    busy = 100.0f * (1.0f - static_cast<float>((idle - prev_idle_ms_) / (dt * 1000.0)));
                prev_idle_ms_ = idle;
            }
            if (busy >= 0) {
                busy = std::clamp(busy, 0.0f, 100.0f);
                st_.gpu = st_.gpu < 0 ? busy : std::clamp(st_.gpu * 0.35f + busy * 0.65f, 0.0f, 100.0f);
            }
        }
        if (!gpu_freq_path_.empty() && read_file(gpu_freq_path_, t)) st_.gpu_mhz = std::atoi(t.c_str());
        if (!gpu_max_path_.empty() && read_file(gpu_max_path_, t)) st_.gpu_max_mhz = std::atoi(t.c_str());
    }

    struct sysinfo si {};
    if (sysinfo(&si) == 0) {
        st_.uptime_s = si.uptime;
        st_.load1 = si.loads[0] / 65536.0f;
    }

    push(cpu_h_, st_.cpu / 100.0f);
    push(ram_h_, st_.ram / 100.0f);
    if (st_.gpu >= 0) push(gpu_h_, st_.gpu / 100.0f);
    float rate = st_.net_down + st_.net_up;
    net_peak_ = std::max(net_peak_ * 0.98f, std::max(rate, 64.0f * 1024));
    push(net_h_, rate / net_peak_);
    st_.cpu_hist.assign(cpu_h_.begin(), cpu_h_.end());
    st_.ram_hist.assign(ram_h_.begin(), ram_h_.end());
    st_.net_hist.assign(net_h_.begin(), net_h_.end());
    st_.gpu_hist.assign(gpu_h_.begin(), gpu_h_.end());
}

}  // namespace island
