#include "services/notifications.hpp"

#include <cstring>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"
#include "services/images.hpp"

namespace island {

using json = nlohmann::json;

static const char* kName = "org.freedesktop.Notifications";
static const char* kPath = "/org/freedesktop/Notifications";

std::string Notification::plain_body() const { return strip_markup(body); }

static const sd_bus_vtable kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CloseNotification", "u", "", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCapabilities", "", "as", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetServerInformation", "", "ssss", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked", "us", 0),
    SD_BUS_VTABLE_END,
};

// sd-bus vtables take plain function pointers; patch ours in at runtime so
// the handlers can stay private statics.
static sd_bus_vtable g_vtable[sizeof(kVtable) / sizeof(kVtable[0])];

Notifications::~Notifications() {
    for (auto& [id, t] : expiry_timers_) Loop::get().cancel(t);
    if (vtable_slot_) sd_bus_slot_unref(vtable_slot_);
}

std::string Notifications::mode_label() const {
    if (owned_) return "servidor";
    if (monitor_) return "monitor";
    return "apagado";
}

bool Notifications::init(sd_bus* bus) {
    bus_ = bus;
    load();
    const std::string& mode = config().notification_mode;
    if (!bus_ || mode == "off") return false;

    if (mode != "monitor") {
        memcpy(g_vtable, kVtable, sizeof(kVtable));
        g_vtable[1].x.method.handler = method_notify;
        g_vtable[2].x.method.handler = method_close;
        g_vtable[3].x.method.handler = method_caps;
        g_vtable[4].x.method.handler = method_info;
        if (sd_bus_add_object_vtable(bus_, &vtable_slot_, kPath, kName, g_vtable, this) < 0) {
            std::cerr << "[notify] cannot export the server object\n";
        }
        sd_bus_match_signal(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "NameAcquired", on_name_acquired, this);
        sd_bus_match_signal(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "NameLost", on_name_lost, this);
        // Queue for the name: if noctalia (or mako…) holds it we take over
        // automatically the moment it lets go.
        uint64_t flags = mode == "server" ? SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT | SD_BUS_NAME_QUEUE
                                          : SD_BUS_NAME_QUEUE | SD_BUS_NAME_ALLOW_REPLACEMENT;
        int r = sd_bus_request_name(bus_, kName, flags);
        owned_ = r > 0;
        std::cout << "[notify] " << (owned_ ? "serving org.freedesktop.Notifications"
                                            : "another daemon owns the name; queued and monitoring")
                  << "\n";
    }
    if (!owned_) start_monitor();
    return true;
}

// ── monitor fallback ───────────────────────────────────────────────────────

void Notifications::start_monitor() {
    if (monitor_ || config().notification_mode == "server") return;
    auto bus = std::make_unique<Bus>();
    if (!bus->open_user()) return;
    const char* rule = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(bus->get(), "org.freedesktop.DBus", "/org/freedesktop/DBus",
                               "org.freedesktop.DBus.Monitoring", "BecomeMonitor", &err, nullptr, "asu", 1, rule, 0);
    if (r < 0) {
        std::cerr << "[notify] BecomeMonitor failed: " << (err.message ? err.message : "") << "\n";
        sd_bus_error_free(&err);
        return;
    }
    sd_bus_add_filter(bus->get(), nullptr, monitor_filter, this);
    monitor_ = std::move(bus);
}

void Notifications::stop_monitor() { monitor_.reset(); }

void Notifications::health_check() {
    if (owned_ || config().notification_mode == "off" || config().notification_mode == "server") return;
    if (monitor_ && sd_bus_is_open(monitor_->get()) > 0) return;
    if (monitor_) std::cerr << "[notify] monitor connection lost, reconnecting\n";
    monitor_.reset();
    start_monitor();
}

int Notifications::monitor_filter(sd_bus_message* m, void* ud, sd_bus_error*) {
    // Always consume (return 1): letting sd-bus dispatch a monitored method
    // call makes it reply with an error, and the bus disconnects monitors
    // that send anything.
    auto* self = static_cast<Notifications*>(ud);
    if (self->owned_) return 1;
    const char* member = sd_bus_message_get_member(m);
    if (!member || strcmp(member, "Notify") != 0) return 1;
    uint32_t replaces = 0;
    NotificationPtr n = parse_notify(m, replaces);
    if (!n) return 1;
    n->monitored = true;
    n->live = false;
    n->id = 0x80000000u | self->next_id_++;
    self->present(n);
    return 1;
}

int Notifications::on_name_acquired(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Notifications*>(ud);
    const char* name = nullptr;
    if (sd_bus_message_read_basic(m, 's', &name) > 0 && name && strcmp(name, kName) == 0) {
        self->owned_ = true;
        self->stop_monitor();
        std::cout << "[notify] acquired org.freedesktop.Notifications\n";
        if (self->on_change) self->on_change();
    }
    return 0;
}

int Notifications::on_name_lost(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Notifications*>(ud);
    const char* name = nullptr;
    if (sd_bus_message_read_basic(m, 's', &name) > 0 && name && strcmp(name, kName) == 0) {
        self->owned_ = false;
        self->start_monitor();
        if (self->on_change) self->on_change();
    }
    return 0;
}

// ── parsing ────────────────────────────────────────────────────────────────

NotificationPtr Notifications::parse_notify(sd_bus_message* m, uint32_t& replaces) {
    const char *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
    if (sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body) < 0) return nullptr;
    auto n = std::make_shared<Notification>();
    n->app = app ? app : "";
    n->app_icon = icon ? icon : "";
    n->summary = strip_markup(summary ? summary : "");
    n->body = sanitize_markup(body ? body : "");
    n->time = now_epoch();

    if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
        const char *key = nullptr, *label = nullptr;
        while (sd_bus_message_read_basic(m, 's', &key) > 0) {
            if (sd_bus_message_read_basic(m, 's', &label) <= 0) break;
            std::string k = key ? key : "", l = label ? label : "";
            if (k == "default") n->has_default = true;
            else if (!l.empty()) n->actions.emplace_back(k, l);
        }
        sd_bus_message_exit_container(m);
    }

    if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read_basic(m, 's', &key);
            std::string k = key ? key : "";
            if (k == "image-data" || k == "image_data" || k == "icon_data") {
                if (sd_bus_message_enter_container(m, 'v', "(iiibiiay)") >= 0) {
                    int32_t w = 0, h = 0, stride = 0, bps = 0, ch = 0;
                    int alpha = 0;
                    const void* data = nullptr;
                    size_t len = 0;
                    if (sd_bus_message_enter_container(m, 'r', "iiibiiay") >= 0) {
                        sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bps, &ch);
                        sd_bus_message_read_array(m, 'y', &data, &len);
                        sd_bus_message_exit_container(m);
                        if (data && bps == 8 && !n->pixels) {
                            cairo_surface_t* s = surface_from_rgba(w, h, stride, alpha != 0, ch,
                                                                   static_cast<const uint8_t*>(data), len, 128);
                            if (s) n->pixels.reset(s, cairo_surface_destroy);
                        }
                    }
                    sd_bus_message_exit_container(m);
                } else {
                    sd_bus_message_skip(m, "v");
                }
            } else {
                Value v;
                read_variant(m, v);
                if (k == "urgency") n->urgency = static_cast<int>(std::clamp<int64_t>(v.integer(1), 0, 2));
                else if (k == "image-path" || k == "image_path") n->image = v.str();
                else if (k == "desktop-entry") n->desktop_entry = v.str();
                else if (k == "transient") n->transient = v.boolean();
                else if (k == "resident") n->resident = v.boolean();
                else if (k == "value") n->value = static_cast<int>(std::clamp<int64_t>(v.integer(-1), -1, 100));
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }

    int32_t timeout = -1;
    sd_bus_message_read_basic(m, 'i', &timeout);
    n->timeout_ms = timeout;

    if (n->image.empty() && !n->app_icon.empty()) {
        if (n->app_icon[0] == '/' || starts_with(n->app_icon, "file://")) n->image = n->app_icon;
        else n->image = "icon:" + n->app_icon;
    }
    if (n->image.empty() && !n->desktop_entry.empty()) n->image = "icon:" + n->desktop_entry;
    if (n->summary.empty() && n->body.empty()) n->summary = n->app;
    return n;
}

int Notifications::method_notify(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Notifications*>(ud);
    uint32_t replaces = 0;
    NotificationPtr n = parse_notify(m, replaces);
    if (!n) return sd_bus_reply_method_return(m, "u", 0);
    n->live = true;
    if (replaces && self->find_by_id(replaces)) {
        n->id = replaces;
    } else {
        n->id = self->next_id_++;
        if (self->next_id_ >= 0x80000000u) self->next_id_ = 1;
    }
    uint32_t id = n->id;
    self->present(n);
    return sd_bus_reply_method_return(m, "u", id);
}

int Notifications::method_close(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Notifications*>(ud);
    uint32_t id = 0;
    sd_bus_message_read_basic(m, 'u', &id);
    if (auto n = self->find_by_id(id)) {
        n->live = false;
        if (self->current_ && self->current_->id == id) self->current_.reset();
        self->emit_closed(id, 3);
        if (self->on_change) self->on_change();
    }
    return sd_bus_reply_method_return(m, "");
}

int Notifications::method_caps(sd_bus_message* m, void*, sd_bus_error*) {
    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    const char* caps[] = {"actions", "body", "body-markup", "icon-static", "persistence", nullptr};
    sd_bus_message_append_strv(reply, const_cast<char**>(caps));
    int r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r;
}

int Notifications::method_info(sd_bus_message* m, void*, sd_bus_error*) {
    return sd_bus_reply_method_return(m, "ssss", "niri-island", "niri-island", "2.0", "1.2");
}

// ── model ──────────────────────────────────────────────────────────────────

NotificationPtr Notifications::find_by_id(uint32_t id) const {
    if (current_ && current_->id == id) return current_;
    for (auto& n : history_)
        if (n->id == id) return n;
    return nullptr;
}

int Notifications::effective_timeout(const Notification& n) const {
    if (n.urgency >= 2) return 0;  // critical waits for the user
    if (n.timeout_ms > 0) return std::min(n.timeout_ms, 15000);
    if (n.timeout_ms == 0) return 15000;  // "never" is capped: no app owns the island forever
    return config().notification_timeout_ms;
}

void Notifications::present(NotificationPtr n) {
    // Replace in place (progress bars, volume notifications from apps).
    bool replaced = false;
    for (auto& h : history_) {
        if (h->id == n->id) {
            h = n;
            replaced = true;
            break;
        }
    }
    if (!replaced && !n->transient) {
        history_.insert(history_.begin(), n);
        size_t limit = static_cast<size_t>(std::max(5, config().notification_history));
        while (history_.size() > limit) {
            auto old = history_.back();
            history_.pop_back();
            if (old->live && owned_) emit_closed(old->id, 1);
        }
    }
    save();

    bool critical = n->urgency >= 2;
    bool show = !(dnd_ && !critical && !n->on_action) && !(current_ && current_->urgency >= 2 && !critical && current_->id != n->id);
    if (show) {
        current_ = n;
        schedule_expiry(n);
        if (on_arrive) on_arrive(n);
    } else if (dnd_ == DndDiscreet && !critical && !replaced) {
        if (on_discreet) on_discreet(n);
    }
    if (on_change) on_change();
}

void Notifications::schedule_expiry(const NotificationPtr& n) {
    auto it = expiry_timers_.find(n->id);
    if (it != expiry_timers_.end()) {
        Loop::get().cancel(it->second);
        expiry_timers_.erase(it);
    }
    int t = effective_timeout(*n);
    if (t <= 0) return;
    uint32_t id = n->id;
    expiry_timers_[id] = Loop::get().after(t / 1000.0, [this, id] {
        expiry_timers_.erase(id);
        if (current_ && current_->id == id) {
            current_.reset();
            if (on_change) on_change();
        }
    });
}

void Notifications::emit_closed(uint32_t id, uint32_t reason) {
    if (!owned_ || (id & 0x80000000u)) return;
    sd_bus_emit_signal(bus_, kPath, kName, "NotificationClosed", "uu", id, reason);
}

void Notifications::dismiss_current() {
    if (!current_) return;
    current_.reset();
    if (on_change) on_change();
}

void Notifications::close(uint32_t id) {
    if (auto n = find_by_id(id)) {
        if (n->live) emit_closed(id, 2);
        n->live = false;
    }
    if (current_ && current_->id == id) current_.reset();
    if (on_change) on_change();
}

bool Notifications::invoke(uint32_t id, const std::string& action) {
    auto n = find_by_id(id);
    if (n && n->on_action) {
        auto f = n->on_action;
        if (current_ && current_->id == id) current_.reset();
        f(action);
        if (on_change) on_change();
        return true;
    }
    if (!n || !n->live || !owned_) return false;
    sd_bus_emit_signal(bus_, kPath, kName, "ActionInvoked", "us", id, action.c_str());
    if (!n->resident) {
        emit_closed(id, 2);
        n->live = false;
    }
    if (current_ && current_->id == id) current_.reset();
    if (on_change) on_change();
    return true;
}

void Notifications::remove(uint32_t id) {
    for (auto it = history_.begin(); it != history_.end(); ++it) {
        if ((*it)->id == id) {
            if ((*it)->live) emit_closed(id, 2);
            history_.erase(it);
            break;
        }
    }
    if (current_ && current_->id == id) current_.reset();
    save();
    if (on_change) on_change();
}

void Notifications::clear_history() {
    for (auto& n : history_)
        if (n->live) emit_closed(n->id, 2);
    history_.clear();
    current_.reset();
    save();
    if (on_change) on_change();
}

const char* Notifications::dnd_label(int mode) {
    return mode == DndDiscreet ? "Discreto" : mode == DndTotal ? "Total" : "Desactivado";
}

void Notifications::set_dnd_mode(int mode) {
    mode = std::clamp(mode, 0, 2);
    bool on = mode != DndOff;
    dnd_ = mode;
    if (on) last_dnd_ = mode;
    if (on && current_ && current_->urgency < 2) current_.reset();
    save_state();
    // When noctalia is the daemon, keep its own DND in step.
    if (!owned_ && proc::has("noctalia")) proc::spawn({"noctalia", "msg", "notification-dnd-set", on ? "on" : "off"});
    if (on_change) on_change();
}

void Notifications::post(NotificationPtr n) {
    n->id = 0x80000000u | next_id_++;
    if (!n->time) n->time = now_epoch();
    present(n);
}

void Notifications::post_local(const std::string& app, const std::string& summary, const std::string& body,
                               const std::string& icon, int urgency, int timeout_ms) {
    auto n = std::make_shared<Notification>();
    n->id = 0x80000000u | next_id_++;
    n->app = app;
    n->summary = summary;
    n->body = escape_markup(body);
    n->image = icon.empty() ? "" : (icon[0] == '/' ? icon : "icon:" + icon);
    n->urgency = urgency;
    n->timeout_ms = timeout_ms;
    n->time = now_epoch();
    present(n);
}

// ── persistence ────────────────────────────────────────────────────────────

void Notifications::load() {
    std::string text;
    if (read_file(state_dir() + "/state.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object()) {
            // Older state files kept a plain bool.
            if (j.contains("dnd_mode") && j["dnd_mode"].is_number_integer()) dnd_ = std::clamp(j["dnd_mode"].get<int>(), 0, 2);
            else dnd_ = j.value("dnd", false) ? DndTotal : DndOff;
            last_dnd_ = std::clamp(j.value("dnd_last", static_cast<int>(DndTotal)), 1, 2);
        }
    }
    if (read_file(state_dir() + "/notifications.json", text)) {
        json j = json::parse(text, nullptr, false);
        if (j.is_object() && j.contains("history") && j["history"].is_array()) {
            for (auto& e : j["history"]) {
                if (!e.is_object()) continue;
                auto n = std::make_shared<Notification>();
                n->id = 0x80000000u | next_id_++;
                n->app = e.value("app", "");
                n->summary = e.value("summary", "");
                n->body = e.value("body", "");
                n->image = e.value("image", "");
                n->urgency = e.value("urgency", 1);
                n->time = e.value("time", (int64_t)0);
                n->desktop_entry = e.value("desktop_entry", "");
                n->live = false;
                history_.push_back(n);
            }
        }
    }
    restored_ = true;
}

void Notifications::save() {
    if (!restored_) return;
    json arr = json::array();
    for (auto& n : history_) {
        arr.push_back({{"app", n->app},
                       {"summary", n->summary},
                       {"body", n->body},
                       {"image", n->image},
                       {"urgency", n->urgency},
                       {"time", n->time},
                       {"desktop_entry", n->desktop_entry}});
    }
    make_dirs(state_dir());
    json j{{"history", arr}};
    write_file_atomic(state_dir() + "/notifications.json", j.dump());
}

void Notifications::save_state() {
    make_dirs(state_dir());
    json j;
    std::string text;
    if (read_file(state_dir() + "/state.json", text)) {
        j = json::parse(text, nullptr, false);
        if (!j.is_object()) j = json::object();
    }
    j["dnd"] = dnd_ != DndOff;
    j["dnd_mode"] = dnd_;
    j["dnd_last"] = last_dnd_;
    write_file_atomic(state_dir() + "/state.json", j.dump(2));
}

}  // namespace island
