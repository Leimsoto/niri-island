#pragma once

#include <cairo.h>
#include <functional>
#include <memory>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

#include "services/dbus.hpp"

namespace island {

struct Notification {
    uint32_t id = 0;
    std::string app, app_icon, desktop_entry;
    std::string summary, body;  // body is sanitized Pango markup
    std::string image;          // ImageCache key ("icon:name", path or URI), when no pixels
    std::vector<std::pair<std::string, std::string>> actions;  // key, label ("default" excluded)
    bool has_default = false;
    int urgency = 1;            // 0 low, 1 normal, 2 critical
    int timeout_ms = 0;         // 0 = until dismissed
    int64_t time = 0;           // epoch
    int value = -1;             // progress hint 0..100
    bool transient = false, resident = false;
    bool live = false;          // still open on our server: actions can be invoked
    bool monitored = false;     // seen through the monitor (another daemon owns it)
    std::shared_ptr<cairo_surface_t> pixels;
    // Notifications posted by the island itself handle their own actions.
    std::function<void(const std::string& action)> on_action;

    std::string plain_body() const;
};

using NotificationPtr = std::shared_ptr<Notification>;

class Notifications {
public:
    ~Notifications();
    bool init(sd_bus* user_bus);

    NotificationPtr current() const { return current_; }
    const std::vector<NotificationPtr>& history() const { return history_; }
    bool server() const { return owned_; }
    std::string mode_label() const;

    // Takes it off the island, without telling the application.
    void dismiss_current();
    // The user closed it: the application is told (reason 2).
    void close(uint32_t id);
    // Invokes an action ("default" for a click) and closes unless resident.
    bool invoke(uint32_t id, const std::string& action);
    void remove(uint32_t id);
    void clear_history();

    // Do not disturb: 0 off, 1 discreet (sound + a small icon beside the
    // island), 2 total (nothing at all). Critical ones always come through.
    enum { DndOff = 0, DndDiscreet = 1, DndTotal = 2 };
    bool dnd() const { return dnd_ != DndOff; }
    int dnd_mode() const { return dnd_; }
    void set_dnd(bool on) { set_dnd_mode(on ? (last_dnd_ ? last_dnd_ : DndTotal) : DndOff); }
    void set_dnd_mode(int mode);
    void cycle_dnd() { set_dnd_mode((dnd_ + 1) % 3); }
    static const char* dnd_label(int mode);

    // Restarts the monitor connection if the bus dropped it.
    void health_check();

    // Posts a notification from the island itself (timers, battery…).
    void post_local(const std::string& app, const std::string& summary, const std::string& body,
                    const std::string& icon, int urgency = 1, int timeout_ms = -1);
    // A fully prepared local notification (pixels, actions, on_action).
    void post(NotificationPtr n);

    std::function<void(const NotificationPtr&)> on_arrive;
    std::function<void(const NotificationPtr&)> on_discreet;  // arrived while discreet
    std::function<void()> on_change;

private:
    sd_bus* bus_ = nullptr;
    sd_bus_slot* vtable_slot_ = nullptr;
    std::unique_ptr<Bus> monitor_;
    bool owned_ = false;
    uint32_t next_id_ = 1;
    NotificationPtr current_;
    std::vector<NotificationPtr> history_;
    std::map<uint32_t, uint64_t> expiry_timers_;
    int dnd_ = DndOff;
    int last_dnd_ = DndTotal;
    bool restored_ = false;

    void present(NotificationPtr n);
    void schedule_expiry(const NotificationPtr& n);
    void emit_closed(uint32_t id, uint32_t reason);
    void start_monitor();
    void stop_monitor();
    void load();
    void save();
    void save_state();
    NotificationPtr find_by_id(uint32_t id) const;
    int effective_timeout(const Notification& n) const;

    static int method_notify(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int method_close(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int method_caps(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int method_info(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int on_name_acquired(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int on_name_lost(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int monitor_filter(sd_bus_message* m, void* ud, sd_bus_error* err);
    static NotificationPtr parse_notify(sd_bus_message* m, uint32_t& replaces);
};

}  // namespace island
