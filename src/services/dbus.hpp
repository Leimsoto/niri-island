#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

namespace island {

// A decoded D-Bus variant: enough for MPRIS metadata, notification hints and
// BlueZ / NetworkManager properties.
struct Value {
    enum class Kind { None, Str, Int, Double, Bool, StrList, Dict } kind = Kind::None;
    std::string s;
    int64_t i = 0;
    double d = 0;
    bool b = false;
    std::vector<std::string> list;
    std::shared_ptr<std::map<std::string, Value>> dict;

    std::string str(const std::string& def = "") const { return kind == Kind::Str ? s : def; }
    int64_t integer(int64_t def = 0) const {
        if (kind == Kind::Int) return i;
        if (kind == Kind::Double) return static_cast<int64_t>(d);
        return def;
    }
    double number(double def = 0) const {
        if (kind == Kind::Double) return d;
        if (kind == Kind::Int) return static_cast<double>(i);
        return def;
    }
    bool boolean(bool def = false) const { return kind == Kind::Bool ? b : def; }
    std::string joined(const char* sep = ", ") const;
};

using Dict = std::map<std::string, Value>;

// Reads one variant ('v') at the current position.
int read_variant(sd_bus_message* m, Value& out);
// Reads a value of an already-entered type signature character.
int read_basic_or_container(sd_bus_message* m, Value& out);
// Reads an a{sv} at the current position.
int read_dict(sd_bus_message* m, Dict& out);

const Value* find(const Dict& d, const char* key);

// Owns one bus connection integrated into the Loop.
class Bus {
public:
    ~Bus();
    bool open_user();
    bool open_system();
    sd_bus* get() const { return bus_; }
    explicit operator bool() const { return bus_ != nullptr; }

    // Processes queued messages and flushes writes; returns the poll timeout in ms (-1 none).
    int pump();

private:
    sd_bus* bus_ = nullptr;
    void attach();
};

}  // namespace island
