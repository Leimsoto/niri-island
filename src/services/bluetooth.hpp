#pragma once

#include <functional>
#include <map>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

#include "services/dbus.hpp"

namespace island {

struct BtDevice {
    std::string path, address, name, icon;
    bool paired = false, connected = false, trusted = false;
    int battery = -1;
    bool busy = false;
};

struct BtState {
    bool available = false;
    bool powered = false;
    bool discovering = false;
    std::string adapter;
    std::vector<BtDevice> devices;   // sorted: connected, paired, others
    std::string message;
};

// BlueZ over the system bus: ObjectManager for the device list, signals for
// changes, async method calls for actions. No bluetoothctl, no threads.
class Bluetooth {
public:
    bool init(sd_bus* system_bus);
    const BtState& state() const { return st_; }

    void set_powered(bool on);
    void start_discovery(double seconds = 12.0);
    void stop_discovery();
    void toggle_connect(const std::string& path);
    void forget(const std::string& path);

    std::function<void()> on_change;
    std::function<void(const BtDevice&, bool connected)> on_device_connection;

private:
    struct Obj {
        Dict adapter;
        Dict device;
        Dict battery;
        bool is_adapter = false, is_device = false;
    };
    sd_bus* sys_ = nullptr;
    std::map<std::string, Obj> objs_;
    BtState st_;
    uint64_t discovery_timer_ = 0;
    std::map<std::string, bool> busy_;

    void load();
    void rebuild();
    void parse_interfaces(sd_bus_message* m, const std::string& path);
    static int on_added(sd_bus_message* m, void* ud, sd_bus_error*);
    static int on_removed(sd_bus_message* m, void* ud, sd_bus_error*);
    static int on_props(sd_bus_message* m, void* ud, sd_bus_error*);
    static int on_owner(sd_bus_message* m, void* ud, sd_bus_error*);
};

}  // namespace island
