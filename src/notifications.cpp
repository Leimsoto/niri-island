#include "notifications.hpp"
#include <iostream>
#include <cstring>

namespace island {

NotificationManager::NotificationManager() = default;

NotificationManager::~NotificationManager() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }
}

int NotificationManager::message_filter(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<NotificationManager*>(userdata);
    if (self->dnd_) return 0;
    const char* member = sd_bus_message_get_member(m);

    if (member && strcmp(member, "Notify") == 0) {
        const char *app = nullptr, *icon = nullptr, *summary = nullptr, *body = nullptr;
        uint32_t replaces_id = 0;

        int r = sd_bus_message_read(m, "susss", &app, &replaces_id, &icon, &summary, &body);
        if (r >= 0) {
            NotificationItem item;
            item.app_name = app ? app : "";
            item.icon = icon ? icon : "";
            item.summary = summary ? summary : "";
            item.body = body ? body : "";
            item.timestamp = std::chrono::steady_clock::now();

            self->current_ = item;

            std::cout << "[Island] Intercepted Notification: " 
                      << item.summary << " (" << item.app_name << ")\n";

            if (self->on_notification) {
                self->on_notification(item);
            }
        }
    }

    return 0;
}

bool NotificationManager::init() {
    int r = sd_bus_default_user(&bus_);
    if (r < 0) {
        std::cerr << "[Notification] Failed to connect to user D-Bus\n";
        return false;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    const char* rule = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";

    r = sd_bus_call_method(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus.Monitoring", "BecomeMonitor",
        &err, nullptr, "asu", 1, rule, 0
    );

    if (r < 0) {
        std::cerr << "[Notification] BecomeMonitor failed: " << (err.message ? err.message : "") << "\n";
        sd_bus_error_free(&err);
        return false;
    }

    sd_bus_add_filter(bus_, nullptr, message_filter, this);
    return true;
}

void NotificationManager::update() {
    if (bus_) {
        while (sd_bus_process(bus_, nullptr) > 0) {}
    }
}

int NotificationManager::get_fd() const {
    return bus_ ? sd_bus_get_fd(bus_) : -1;
}

} // namespace island
