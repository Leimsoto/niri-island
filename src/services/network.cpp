#include "services/network.hpp"

#include <memory>
#include <unordered_set>

#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"

namespace island {

std::vector<std::string> nmcli_fields(const std::string& line) {
    // nmcli -t escapes ':' and '\' inside values with a backslash.
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\\' && i + 1 < line.size()) {
            cur += line[++i];
        } else if (c == ':') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool Network::init(sd_bus* sys) {
    sys_ = sys;
    if (sys_) {
        // Any NetworkManager state change: refresh, debounced.
        sd_bus_match_signal(sys_, nullptr, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                            "org.freedesktop.DBus.Properties", "PropertiesChanged", on_nm_signal, this);
        sd_bus_match_signal(sys_, nullptr, "org.freedesktop.NetworkManager", "/org/freedesktop/NetworkManager",
                            "org.freedesktop.NetworkManager", "StateChanged", on_nm_signal, this);
    }
    refresh();
    return true;
}

int Network::on_nm_signal(sd_bus_message*, void* ud, sd_bus_error*) {
    auto* self = static_cast<Network*>(ud);
    Loop::get().cancel(self->debounce_);
    self->debounce_ = Loop::get().after(0.4, [self] {
        self->debounce_ = 0;
        self->refresh();
    });
    return 0;
}

void Network::refresh() {
    if (refreshing_ || !proc::has("nmcli")) return;
    refreshing_ = true;
    auto out = std::make_shared<NetState>();
    auto dev = std::make_shared<std::string>();
    Loop::get().async(
        [out, dev] {
            auto radio = proc::run({"nmcli", "-t", "-f", "WIFI-HW,WIFI", "radio"}, 4000);
            if (radio.ok()) {
                auto f = nmcli_fields(trim(radio.out));
                if (f.size() >= 2) {
                    out->wifi_hw = f[0] == "enabled";
                    out->wifi_enabled = f[1] == "enabled";
                }
            }
            auto devs = proc::run({"nmcli", "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status"}, 4000);
            for (auto& line : split(devs.out, '\n')) {
                auto f = nmcli_fields(line);
                if (f.size() < 4) continue;
                bool connected = starts_with(f[2], "connected") && f[2].find("disconnected") == std::string::npos &&
                                 f[2].find("connecting") == std::string::npos;
                if (f[1] == "wifi") {
                    if (dev->empty()) *dev = f[0];
                    if (connected) out->ssid = f[3];
                } else if (f[1] == "ethernet" && connected) {
                    out->ethernet = true;
                } else if ((f[1] == "wireguard" || f[1] == "tun" || f[1] == "vpn") && connected) {
                    out->vpn = true;
                    out->vpn_name = f[3];
                }
            }
            if (!out->ssid.empty()) {
                auto list = proc::run({"nmcli", "-t", "-f", "IN-USE,SIGNAL", "device", "wifi", "list", "--rescan", "no"},
                                      4000);
                for (auto& line : split(list.out, '\n')) {
                    auto f = nmcli_fields(line);
                    if (f.size() >= 2 && f[0] == "*") {
                        out->signal = std::atoi(f[1].c_str());
                        break;
                    }
                }
            }
            auto conn = proc::run({"nmcli", "-t", "-f", "CONNECTIVITY", "general"}, 3000);
            out->online = trim(conn.out) == "full";
        },
        [this, out, dev] {
            refreshing_ = false;
            st_.wifi_enabled = out->wifi_enabled;
            st_.wifi_hw = out->wifi_hw;
            st_.ssid = out->ssid;
            st_.signal = out->signal;
            st_.ethernet = out->ethernet;
            st_.vpn = out->vpn;
            st_.vpn_name = out->vpn_name;
            st_.online = out->online;
            if (!dev->empty()) wifi_dev_ = *dev;
            if (!st_.wifi_enabled) st_.networks.clear();
            changed();
        });
}

void Network::scan(bool rescan) {
    if (!proc::has("nmcli") || st_.scanning || (st_.listing && !rescan)) return;
    if (rescan) st_.scanning = true;
    else st_.listing = true;
    changed();
    auto list = std::make_shared<std::vector<WifiNetwork>>();
    Loop::get().async(
        [list, rescan] {
            std::unordered_set<std::string> saved;
            auto conns = proc::run({"nmcli", "-t", "-f", "NAME,TYPE", "connection", "show"}, 4000);
            for (auto& line : split(conns.out, '\n')) {
                auto f = nmcli_fields(line);
                if (f.size() >= 2 && f[1] == "802-11-wireless") saved.insert(f[0]);
            }
            // The cached list answers instantly; only rescan when asked or when it is empty.
            auto list_cmd = [](const char* mode) {
                return std::vector<std::string>{"nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY,BSSID,CHAN,FREQ,RATE",
                                                "device", "wifi", "list", "--rescan", mode};
            };
            auto r = proc::run(list_cmd(rescan ? "yes" : "no"), rescan ? 20000 : 4000);
            if (!rescan && trim(r.out).empty()) r = proc::run(list_cmd("auto"), 12000);
            std::unordered_set<std::string> seen;
            std::vector<WifiNetwork> connected, known, other;
            for (auto& line : split(r.out, '\n')) {
                auto f = nmcli_fields(line);
                if (f.size() < 8 || f[1].empty()) continue;
                WifiNetwork n;
                n.in_use = f[0] == "*";
                n.ssid = f[1];
                n.signal = std::atoi(f[2].c_str());
                n.security = (f[3] == "--" || f[3].empty()) ? "" : f[3];
                n.bssid = f[4];
                n.channel = f[5];
                n.freq = f[6];
                n.rate = f[7];
                n.saved = saved.count(n.ssid) > 0;
                if (seen.count(n.ssid)) {
                    // Strongest access point per SSID; keep the in-use flag.
                    auto upgrade = [&](std::vector<WifiNetwork>& v) {
                        for (auto& e : v)
                            if (e.ssid == n.ssid) {
                                e.in_use |= n.in_use;
                                if (n.signal > e.signal) e.signal = n.signal;
                            }
                    };
                    upgrade(connected);
                    upgrade(known);
                    upgrade(other);
                    continue;
                }
                seen.insert(n.ssid);
                (n.in_use ? connected : n.saved ? known : other).push_back(std::move(n));
            }
            auto by_signal = [](const WifiNetwork& a, const WifiNetwork& b) { return a.signal > b.signal; };
            std::sort(known.begin(), known.end(), by_signal);
            std::sort(other.begin(), other.end(), by_signal);
            for (auto* v : {&connected, &known, &other})
                for (auto& n : *v) list->push_back(std::move(n));
        },
        [this, list, rescan] {
            if (rescan) st_.scanning = false;
            else st_.listing = false;
            // Never blank the list on a transient empty answer.
            if (!list->empty() || !st_.wifi_enabled) st_.networks = std::move(*list);
            changed();
        });
}

void Network::set_wifi(bool on) {
    st_.wifi_enabled = on;
    if (!on) st_.networks.clear();
    changed();
    Loop::get().async([on] { proc::run({"nmcli", "radio", "wifi", on ? "on" : "off"}, 8000); },
                      [this, on] {
                          refresh();
                          if (on) Loop::get().after(1.5, [this] { scan(false); });
                      });
}

bool Network::needs_password(const std::string& ssid) const {
    for (auto& n : st_.networks)
        if (n.ssid == ssid) return !n.saved && !n.security.empty();
    return false;
}

void Network::connect(const std::string& ssid, const std::string& password) {
    if (ssid.empty() || st_.busy) return;
    bool saved = false;
    for (auto& n : st_.networks)
        if (n.ssid == ssid) saved = n.saved;
    if (password.empty() && needs_password(ssid)) {
        if (on_password_needed) on_password_needed(ssid);
        return;
    }
    st_.busy = true;
    st_.connecting_ssid = ssid;
    st_.message = "Conectando a " + ssid + "…";
    changed();
    auto res = std::make_shared<proc::Result>();
    Loop::get().async(
        [res, ssid, password, saved] {
            if (saved && password.empty()) {
                *res = proc::run({"nmcli", "--wait", "20", "connection", "up", "id", ssid}, 25000, nullptr, true);
            } else if (password.empty()) {
                *res = proc::run({"nmcli", "--wait", "20", "device", "wifi", "connect", ssid}, 25000, nullptr, true);
            } else {
                *res = proc::run({"nmcli", "--wait", "20", "device", "wifi", "connect", ssid, "password", password},
                                 25000, nullptr, true);
            }
            if (!res->ok() && !saved) {
                // A failed first attempt leaves a broken profile behind.
                proc::run({"nmcli", "connection", "delete", "id", ssid}, 5000);
            }
        },
        [this, res, ssid] {
            st_.busy = false;
            st_.connecting_ssid.clear();
            std::string err = lower(res->out);
            if (res->ok()) {
                st_.message.clear();
                if (on_connected) on_connected(ssid);
            } else if (err.find("secrets were required") != std::string::npos || err.find("psk") != std::string::npos ||
                       err.find("password") != std::string::npos || err.find("802-1x") != std::string::npos ||
                       err.find("authentication") != std::string::npos) {
                st_.message = "Contraseña incorrecta";
                if (on_password_needed) on_password_needed(ssid);
            } else if (err.find("no network with ssid") != std::string::npos) {
                st_.message = "Red fuera de alcance";
            } else if (err.find("timeout") != std::string::npos || err.find("timed out") != std::string::npos) {
                st_.message = "Tiempo de espera agotado";
            } else {
                st_.message = "No se pudo conectar";
            }
            refresh();
            scan(false);
        });
}

void Network::disconnect() {
    if (wifi_dev_.empty() || st_.busy) return;
    st_.busy = true;
    st_.message = "Desconectando…";
    changed();
    std::string dev = wifi_dev_;
    Loop::get().async([dev] { proc::run({"nmcli", "device", "disconnect", dev}, 10000); },
                      [this] {
                          st_.busy = false;
                          st_.message.clear();
                          refresh();
                          scan(false);
                      });
}

void Network::forget(const std::string& ssid) {
    st_.message = "Olvidando " + ssid + "…";
    changed();
    Loop::get().async([ssid] { proc::run({"nmcli", "connection", "delete", "id", ssid}, 8000); },
                      [this] {
                          st_.message.clear();
                          refresh();
                          scan(false);
                      });
}

void Network::load_details(const WifiNetwork& net, std::function<void(const WifiDetails&)> done) {
    auto d = std::make_shared<WifiDetails>();
    d->net = net;
    std::string dev = wifi_dev_;
    Loop::get().async(
        [d, dev] {
            if (d->net.saved || d->net.in_use) {
                auto r = proc::run({"nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", "id",
                                    d->net.ssid},
                                   5000);
                if (r.ok()) d->password = trim(r.out);
            }
            if (d->net.in_use && !dev.empty()) {
                auto r = proc::run({"nmcli", "-t", "-f", "IP4.ADDRESS,IP4.GATEWAY,IP4.DNS,GENERAL.HWADDR", "device", "show",
                                    dev},
                                   5000);
                for (auto& line : split(r.out, '\n')) {
                    size_t c = line.find(':');
                    if (c == std::string::npos) continue;
                    std::string k = line.substr(0, c);
                    std::string v = replace_all(line.substr(c + 1), "\\:", ":");
                    if (starts_with(k, "IP4.ADDRESS") && d->ip.empty()) d->ip = v;
                    else if (k == "IP4.GATEWAY") d->gateway = v;
                    else if (starts_with(k, "IP4.DNS") && d->dns.empty()) d->dns = v;
                    else if (k == "GENERAL.HWADDR") d->mac = v;
                }
            }
            d->loaded = true;
        },
        [d, done] { done(*d); });
}

}  // namespace island
