#include "services/bluetooth.hpp"

#include <cstring>
#include <iostream>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

static const char* kBluez = "org.bluez";

bool Bluetooth::init(sd_bus* sys) {
    sys_ = sys;
    if (!sys_) return false;
    sd_bus_match_signal(sys_, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", on_added,
                        this);
    sd_bus_match_signal(sys_, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                        on_removed, this);
    sd_bus_add_match(sys_, nullptr,
                     "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
                     "member='PropertiesChanged',path_namespace='/org/bluez'",
                     on_props, this);
    sd_bus_match_signal(sys_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner, this);
    load();
    return true;
}

void Bluetooth::load() {
    sd_bus_call_method_async(
        sys_, nullptr, kBluez, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
            auto* self = static_cast<Bluetooth*>(ud);
            self->objs_.clear();
            if (sd_bus_message_is_method_error(m, nullptr)) {
                self->st_ = BtState{};
                if (self->on_change) self->on_change();
                return 0;
            }
            if (sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}") < 0) return 0;
            while (sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
                const char* path = nullptr;
                sd_bus_message_read_basic(m, 'o', &path);
                self->parse_interfaces(m, path ? path : "");
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
            self->rebuild();
            return 0;
        },
        this, "");
}

void Bluetooth::parse_interfaces(sd_bus_message* m, const std::string& path) {
    if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) return;
    while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
        const char* iface = nullptr;
        sd_bus_message_read_basic(m, 's', &iface);
        Dict props;
        read_dict(m, props);
        std::string i = iface ? iface : "";
        if (i == "org.bluez.Adapter1") {
            objs_[path].is_adapter = true;
            objs_[path].adapter = std::move(props);
        } else if (i == "org.bluez.Device1") {
            objs_[path].is_device = true;
            objs_[path].device = std::move(props);
        } else if (i == "org.bluez.Battery1") {
            objs_[path].battery = std::move(props);
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
}

void Bluetooth::rebuild() {
    BtState s;
    s.message = st_.message;
    for (auto& [path, o] : objs_) {
        if (o.is_adapter && s.adapter.empty()) {
            s.available = true;
            s.adapter = path;
            if (auto v = find(o.adapter, "Powered")) s.powered = v->boolean();
            if (auto v = find(o.adapter, "Discovering")) s.discovering = v->boolean();
        }
    }
    std::vector<BtDevice> connected, paired, other;
    for (auto& [path, o] : objs_) {
        if (!o.is_device) continue;
        BtDevice d;
        d.path = path;
        if (auto v = find(o.device, "Address")) d.address = v->str();
        if (auto v = find(o.device, "Alias")) d.name = v->str();
        if (d.name.empty())
            if (auto v = find(o.device, "Name")) d.name = v->str();
        if (auto v = find(o.device, "Icon")) d.icon = v->str();
        if (auto v = find(o.device, "Paired")) d.paired = v->boolean();
        if (auto v = find(o.device, "Connected")) d.connected = v->boolean();
        if (auto v = find(o.device, "Trusted")) d.trusted = v->boolean();
        if (auto v = find(o.battery, "Percentage")) d.battery = static_cast<int>(v->integer(-1));
        d.busy = busy_.count(path) > 0;
        // Nameless devices found during discovery are noise.
        bool has_name = !d.name.empty() && d.name != d.address && replace_all(d.name, "-", ":") != d.address;
        if (!has_name && !d.paired) continue;
        if (d.name.empty()) d.name = d.address;
        (d.connected ? connected : d.paired ? paired : other).push_back(std::move(d));
    }
    auto by_name = [](const BtDevice& a, const BtDevice& b) { return lower(a.name) < lower(b.name); };
    std::sort(paired.begin(), paired.end(), by_name);
    std::sort(other.begin(), other.end(), by_name);
    for (auto* v : {&connected, &paired, &other})
        for (auto& d : *v) s.devices.push_back(std::move(d));

    // Connection edges, for a small toast ("AirPods conectados").
    if (on_device_connection) {
        for (auto& d : s.devices) {
            for (auto& old : st_.devices) {
                if (old.path == d.path && old.connected != d.connected) on_device_connection(d, d.connected);
            }
        }
    }
    st_ = std::move(s);
    if (on_change) on_change();
}

int Bluetooth::on_added(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Bluetooth*>(ud);
    const char* path = nullptr;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0 || !path) return 0;
    self->parse_interfaces(m, path);
    self->rebuild();
    return 0;
}

int Bluetooth::on_removed(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Bluetooth*>(ud);
    const char* path = nullptr;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0 || !path) return 0;
    std::vector<std::string> ifaces;
    if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
        const char* s;
        while (sd_bus_message_read_basic(m, 's', &s) > 0) ifaces.emplace_back(s);
        sd_bus_message_exit_container(m);
    }
    auto it = self->objs_.find(path);
    if (it != self->objs_.end()) {
        for (auto& i : ifaces) {
            if (i == "org.bluez.Device1") it->second.is_device = false;
            if (i == "org.bluez.Adapter1") it->second.is_adapter = false;
            if (i == "org.bluez.Battery1") it->second.battery.clear();
        }
        if (!it->second.is_device && !it->second.is_adapter) self->objs_.erase(it);
    }
    self->rebuild();
    return 0;
}

int Bluetooth::on_props(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Bluetooth*>(ud);
    const char* path = sd_bus_message_get_path(m);
    const char* iface = nullptr;
    if (!path || sd_bus_message_read_basic(m, 's', &iface) < 0 || !iface) return 0;
    Dict props;
    if (read_dict(m, props) < 0) return 0;
    Obj& o = self->objs_[path];
    Dict* target = nullptr;
    if (strcmp(iface, "org.bluez.Adapter1") == 0) {
        o.is_adapter = true;
        target = &o.adapter;
    } else if (strcmp(iface, "org.bluez.Device1") == 0) {
        o.is_device = true;
        target = &o.device;
    } else if (strcmp(iface, "org.bluez.Battery1") == 0) {
        target = &o.battery;
    }
    if (!target) return 0;
    for (auto& [k, v] : props) (*target)[k] = v;
    self->rebuild();
    return 0;
}

int Bluetooth::on_owner(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Bluetooth*>(ud);
    const char *name = nullptr, *o = nullptr, *n = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &o, &n) < 0 || !name || strcmp(name, kBluez) != 0) return 0;
    self->objs_.clear();
    if (n && *n) self->load();
    else self->rebuild();
    return 0;
}

void Bluetooth::set_powered(bool on) {
    if (st_.adapter.empty()) return;
    st_.powered = on;
    if (on_change) on_change();
    sd_bus_call_method_async(sys_, nullptr, kBluez, st_.adapter.c_str(), "org.freedesktop.DBus.Properties", "Set",
                             nullptr, nullptr, "ssv", "org.bluez.Adapter1", "Powered", "b", on ? 1 : 0);
}

void Bluetooth::start_discovery(double seconds) {
    if (st_.adapter.empty() || !st_.powered) return;
    sd_bus_call_method_async(sys_, nullptr, kBluez, st_.adapter.c_str(), "org.bluez.Adapter1", "StartDiscovery",
                             nullptr, nullptr, "");
    Loop::get().cancel(discovery_timer_);
    discovery_timer_ = Loop::get().after(seconds, [this] {
        discovery_timer_ = 0;
        stop_discovery();
    });
}

void Bluetooth::stop_discovery() {
    if (st_.adapter.empty()) return;
    sd_bus_call_method_async(sys_, nullptr, kBluez, st_.adapter.c_str(), "org.bluez.Adapter1", "StopDiscovery",
                             nullptr, nullptr, "");
}

namespace {
struct ActionCtx {
    Bluetooth* self;
    std::string path;
    std::string what;
};
}  // namespace

void Bluetooth::toggle_connect(const std::string& path) {
    const BtDevice* dev = nullptr;
    for (auto& d : st_.devices)
        if (d.path == path) dev = &d;
    if (!dev || busy_.count(path)) return;
    busy_[path] = true;
    std::string name = dev->name;
    bool connected = dev->connected, paired = dev->paired;
    st_.message = (connected ? "Desconectando " : "Conectando ") + name + "…";
    rebuild();

    auto finish = [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
        auto* c = static_cast<ActionCtx*>(ud);
        c->self->busy_.erase(c->path);
        if (sd_bus_message_is_method_error(m, nullptr)) {
            const sd_bus_error* e = sd_bus_message_get_error(m);
            c->self->st_.message = "No se pudo " + c->what + (e && e->message ? std::string(": ") + e->message : "");
        } else {
            c->self->st_.message.clear();
        }
        c->self->rebuild();
        delete c;
        return 0;
    };

    if (connected) {
        sd_bus_call_method_async(sys_, nullptr, kBluez, path.c_str(), "org.bluez.Device1", "Disconnect", finish,
                                 new ActionCtx{this, path, "desconectar"}, "");
    } else if (!paired) {
        // Pair first, trust so it reconnects by itself, then connect.
        sd_bus_call_method_async(
            sys_, nullptr, kBluez, path.c_str(), "org.bluez.Device1", "Pair",
            [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
                auto* c = static_cast<ActionCtx*>(ud);
                if (sd_bus_message_is_method_error(m, nullptr)) {
                    c->self->busy_.erase(c->path);
                    c->self->st_.message = "Emparejamiento fallido";
                    c->self->rebuild();
                    delete c;
                    return 0;
                }
                sd_bus_call_method_async(c->self->sys_, nullptr, kBluez, c->path.c_str(),
                                         "org.freedesktop.DBus.Properties", "Set", nullptr, nullptr, "ssv",
                                         "org.bluez.Device1", "Trusted", "b", 1);
                sd_bus_call_method_async(
                    c->self->sys_, nullptr, kBluez, c->path.c_str(), "org.bluez.Device1", "Connect",
                    [](sd_bus_message* r, void* u, sd_bus_error*) -> int {
                        auto* cc = static_cast<ActionCtx*>(u);
                        cc->self->busy_.erase(cc->path);
                        cc->self->st_.message = sd_bus_message_is_method_error(r, nullptr) ? "No se pudo conectar" : "";
                        cc->self->rebuild();
                        delete cc;
                        return 0;
                    },
                    c, "");
                return 0;
            },
            new ActionCtx{this, path, "emparejar"}, "");
    } else {
        sd_bus_call_method_async(sys_, nullptr, kBluez, path.c_str(), "org.bluez.Device1", "Connect", finish,
                                 new ActionCtx{this, path, "conectar"}, "");
    }
}

void Bluetooth::forget(const std::string& path) {
    if (st_.adapter.empty()) return;
    sd_bus_call_method_async(sys_, nullptr, kBluez, st_.adapter.c_str(), "org.bluez.Adapter1", "RemoveDevice", nullptr,
                             nullptr, "o", path.c_str());
}

}  // namespace island
