#pragma once

#include <cstdint>

#include <functional>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

namespace island {

struct WifiNetwork {
    std::string ssid, security, bssid, channel, freq, rate;
    int signal = 0;
    bool in_use = false;
    bool saved = false;
};

struct WifiDetails {
    WifiNetwork net;
    std::string ip, gateway, dns, mac, password;
    bool loaded = false;
};

struct NetState {
    bool wifi_enabled = true;
    bool wifi_hw = true;
    std::string ssid;            // connected Wi-Fi
    int signal = 0;
    bool ethernet = false;
    bool vpn = false;
    std::string vpn_name;
    bool online = false;
    std::vector<WifiNetwork> networks;
    bool scanning = false;
    bool listing = false;        // first list on its way
    bool busy = false;           // connecting / disconnecting
    std::string message;         // last status line for the panel
    std::string connecting_ssid;
};

class Network {
public:
    bool init(sd_bus* system_bus);
    const NetState& state() const { return st_; }

    void refresh();                         // status (cheap)
    void scan(bool rescan);                 // list networks
    void set_wifi(bool on);
    void connect(const std::string& ssid, const std::string& password = "");
    void disconnect();
    void forget(const std::string& ssid);
    void load_details(const WifiNetwork& net, std::function<void(const WifiDetails&)> done);

    bool needs_password(const std::string& ssid) const;
    std::function<void()> on_change;
    std::function<void(const std::string& ssid)> on_password_needed;
    std::function<void(const std::string& ssid)> on_connected;

private:
    sd_bus* sys_ = nullptr;
    NetState st_;
    bool refreshing_ = false;
    uint64_t debounce_ = 0;
    std::string wifi_dev_;

    static int on_nm_signal(sd_bus_message* m, void* ud, sd_bus_error*);
    void changed() {
        if (on_change) on_change();
    }
};

std::vector<std::string> nmcli_fields(const std::string& line);

}  // namespace island
