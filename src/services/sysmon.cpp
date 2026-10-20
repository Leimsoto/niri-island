#include "services/sysmon.hpp"

#include <cstdio>
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

    struct sysinfo si {};
    if (sysinfo(&si) == 0) {
        st_.uptime_s = si.uptime;
        st_.load1 = si.loads[0] / 65536.0f;
    }

    push(cpu_h_, st_.cpu / 100.0f);
    push(ram_h_, st_.ram / 100.0f);
    float rate = st_.net_down + st_.net_up;
    net_peak_ = std::max(net_peak_ * 0.98f, std::max(rate, 64.0f * 1024));
    push(net_h_, rate / net_peak_);
    st_.cpu_hist.assign(cpu_h_.begin(), cpu_h_.end());
    st_.ram_hist.assign(ram_h_.begin(), ram_h_.end());
    st_.net_hist.assign(net_h_.begin(), net_h_.end());
}

}  // namespace island
