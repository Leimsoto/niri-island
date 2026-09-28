#pragma once

#include <string>
#include <chrono>
#include <functional>
#include <systemd/sd-bus.h>

namespace island {

struct NotificationItem {
    std::string app_name;
    std::string summary;
    std::string body;
    std::string icon;
    std::chrono::steady_clock::time_point timestamp;

    bool is_active(float timeout_sec = 4.5f) const {
        if (summary.empty() && body.empty()) return false;
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - timestamp).count();
        return elapsed < timeout_sec;
    }
};

class NotificationManager {
public:
    NotificationManager();
    ~NotificationManager();

    bool init();
    void update();
    int get_fd() const;

    const NotificationItem& current() const { return current_; }
    bool has_active() const { return current_.is_active(); }
    void dismiss() { current_ = NotificationItem{}; }

    bool dnd() const { return dnd_; }
    void toggle_dnd() { dnd_ = !dnd_; }

    std::function<void(const NotificationItem& item)> on_notification;

private:
    sd_bus* bus_ = nullptr;
    NotificationItem current_;
    bool dnd_ = false;

    static int message_filter(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
};

} // namespace island
