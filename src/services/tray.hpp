#pragma once

#include <cairo.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

namespace island {

// One entry of an app's menu (com.canonical.dbusmenu).
struct TrayMenuItem {
    int id = 0;
    std::string label;
    bool enabled = true, visible = true, separator = false;
    int toggle = 0;  // 0 none, 1 checkbox, 2 radio
    bool checked = false;
    std::vector<TrayMenuItem> children;
};

struct TrayItem {
    std::string key;           // service + path
    std::string service, path;
    std::string id, title, status, icon_name, theme_path, menu_path;
    std::string proc;          // the owner's process name (from the bus)
    std::string name;          // what to show: the title, or the id tidied up
    bool item_is_menu = false;
    std::shared_ptr<cairo_surface_t> pixmap;  // when the app sends pixels, not a name
};

// The system tray: the island is the StatusNotifierWatcher (and its host),
// so apps like Discord, Steam or nm-applet can put their icon in it.
class Tray {
public:
    ~Tray();
    bool init(sd_bus* user_bus);

    const std::vector<TrayItem>& items() const { return items_; }

    void activate(const std::string& key, int x = 0, int y = 0);
    void secondary(const std::string& key, int x = 0, int y = 0);
    void scroll(const std::string& key, int delta);
    // Fetches the menu; `done` gets it (empty if the app has none).
    void menu(const std::string& key, std::function<void(std::vector<TrayMenuItem>)> done);
    void menu_click(const std::string& key, int id);

    std::function<void()> on_change;

private:
    sd_bus* bus_ = nullptr;
    sd_bus_slot* vtable_slot_ = nullptr;
    std::vector<TrayItem> items_;

    TrayItem* find(const std::string& key);
    void add(const std::string& service, const std::string& path);
    void remove_service(const std::string& service);
    void refresh(const std::string& key);
    void emit(const char* signal, const std::string& arg);

    static int m_register_item(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int m_register_host(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int p_items(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply,
                       void* ud, sd_bus_error* err);
    static int p_host(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply,
                      void* ud, sd_bus_error* err);
    static int p_version(sd_bus* bus, const char* path, const char* iface, const char* prop, sd_bus_message* reply,
                         void* ud, sd_bus_error* err);
    static int on_owner_changed(sd_bus_message* m, void* ud, sd_bus_error* err);
    static int on_item_signal(sd_bus_message* m, void* ud, sd_bus_error* err);
};

}  // namespace island
