#include "services/dbus.hpp"

#include <poll.h>
#include <iostream>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

std::string Value::joined(const char* sep) const {
    if (kind == Kind::Str) return s;
    std::string out;
    for (auto& x : list) {
        if (x.empty()) continue;
        if (!out.empty()) out += sep;
        out += x;
    }
    return out;
}

const Value* find(const Dict& d, const char* key) {
    auto it = d.find(key);
    return it == d.end() ? nullptr : &it->second;
}

int read_basic_or_container(sd_bus_message* m, Value& out) {
    char type = 0;
    const char* contents = nullptr;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r <= 0) return r;
    switch (type) {
        case 's':
        case 'o':
        case 'g': {
            const char* v = nullptr;
            r = sd_bus_message_read_basic(m, type, &v);
            out.kind = Value::Kind::Str;
            out.s = v ? v : "";
            return r;
        }
        case 'b': {
            int v = 0;
            r = sd_bus_message_read_basic(m, 'b', &v);
            out.kind = Value::Kind::Bool;
            out.b = v != 0;
            return r;
        }
        case 'y': {
            uint8_t v = 0;
            r = sd_bus_message_read_basic(m, 'y', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 'n': {
            int16_t v = 0;
            r = sd_bus_message_read_basic(m, 'n', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 'q': {
            uint16_t v = 0;
            r = sd_bus_message_read_basic(m, 'q', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 'i': {
            int32_t v = 0;
            r = sd_bus_message_read_basic(m, 'i', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 'u': {
            uint32_t v = 0;
            r = sd_bus_message_read_basic(m, 'u', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 'x': {
            int64_t v = 0;
            r = sd_bus_message_read_basic(m, 'x', &v);
            out.kind = Value::Kind::Int;
            out.i = v;
            return r;
        }
        case 't': {
            uint64_t v = 0;
            r = sd_bus_message_read_basic(m, 't', &v);
            out.kind = Value::Kind::Int;
            out.i = static_cast<int64_t>(v);
            return r;
        }
        case 'd': {
            double v = 0;
            r = sd_bus_message_read_basic(m, 'd', &v);
            out.kind = Value::Kind::Double;
            out.d = v;
            return r;
        }
        case 'v': return read_variant(m, out);
        case 'a': {
            std::string sig = contents ? contents : "";
            if (sig == "s" || sig == "o") {
                r = sd_bus_message_enter_container(m, 'a', sig.c_str());
                if (r < 0) return r;
                out.kind = Value::Kind::StrList;
                const char* v = nullptr;
                while ((r = sd_bus_message_read_basic(m, sig[0], &v)) > 0) out.list.emplace_back(v ? v : "");
                sd_bus_message_exit_container(m);
                return 1;
            }
            if (sig == "{sv}") {
                out.kind = Value::Kind::Dict;
                out.dict = std::make_shared<Dict>();
                return read_dict(m, *out.dict);
            }
            std::string full = "a" + sig;
            return sd_bus_message_skip(m, full.c_str());
        }
        default: {
            std::string sig(1, type);
            if (type == 'r' || type == 'e') sig = std::string(type == 'r' ? "(" : "{") + (contents ? contents : "") +
                                                  (type == 'r' ? ")" : "}");
            return sd_bus_message_skip(m, sig.c_str());
        }
    }
}

int read_variant(sd_bus_message* m, Value& out) {
    char type = 0;
    const char* contents = nullptr;
    int r = sd_bus_message_peek_type(m, &type, &contents);
    if (r <= 0) return r;
    if (type != 'v') return read_basic_or_container(m, out);
    r = sd_bus_message_enter_container(m, 'v', contents);
    if (r < 0) return r;
    r = read_basic_or_container(m, out);
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

int read_dict(sd_bus_message* m, Dict& out) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char* key = nullptr;
        r = sd_bus_message_read_basic(m, 's', &key);
        if (r < 0) return r;
        Value v;
        r = read_variant(m, v);
        if (r < 0) return r;
        out[key ? key : ""] = std::move(v);
        r = sd_bus_message_exit_container(m);
        if (r < 0) return r;
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

Bus::~Bus() {
    if (bus_) {
        Loop::get().unwatch(sd_bus_get_fd(bus_));
        sd_bus_flush_close_unref(bus_);
    }
}

bool Bus::open_user() {
    if (sd_bus_open_user(&bus_) < 0) {
        bus_ = nullptr;
        return false;
    }
    attach();
    return true;
}

bool Bus::open_system() {
    if (sd_bus_open_system(&bus_) < 0) {
        bus_ = nullptr;
        return false;
    }
    attach();
    return true;
}

void Bus::attach() {
    Loop::get().watch(sd_bus_get_fd(bus_), POLLIN, [this](short) { pump(); });
}

int Bus::pump() {
    if (!bus_) return -1;
    int r;
    int guard = 0;
    while ((r = sd_bus_process(bus_, nullptr)) > 0 && ++guard < 512) {}
    if (r < 0 && r != -ECONNRESET) {
        // Connection broke (bus restarted): nothing sensible to do but stop using it.
    }
    sd_bus_flush(bus_);
    uint64_t usec = 0;
    if (sd_bus_get_timeout(bus_, &usec) >= 0 && usec != UINT64_MAX) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
        if (usec <= now) return 0;
        return static_cast<int>(std::min<uint64_t>((usec - now) / 1000 + 1, 60000));
    }
    return -1;
}

}  // namespace island
