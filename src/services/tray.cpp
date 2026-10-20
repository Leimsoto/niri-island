#include "services/tray.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <unistd.h>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

namespace {

const char* kWatcher = "org.kde.StatusNotifierWatcher";
const char* kWatcherPath = "/StatusNotifierWatcher";
const char* kItem = "org.kde.StatusNotifierItem";
const char* kMenu = "com.canonical.dbusmenu";

const sd_bus_vtable kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", nullptr, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", nullptr, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", nullptr, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i", nullptr, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
    SD_BUS_VTABLE_END,
};
sd_bus_vtable g_vtable[sizeof(kVtable) / sizeof(kVtable[0])];

// The largest of an a(iiay) of ARGB32 (network order) pictures, as a cairo surface.
std::shared_ptr<cairo_surface_t> read_pixmap(sd_bus_message* m) {
    std::shared_ptr<cairo_surface_t> best;
    int best_w = 0;
    if (sd_bus_message_enter_container(m, 'a', "(iiay)") < 0) return best;
    while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
        int w = 0, h = 0;
        const void* data = nullptr;
        size_t len = 0;
        sd_bus_message_read(m, "ii", &w, &h);
        sd_bus_message_read_array(m, 'y', &data, &len);
        sd_bus_message_exit_container(m);
        if (w <= 0 || h <= 0 || len < static_cast<size_t>(w) * h * 4 || w <= best_w) continue;
        cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        unsigned char* dst = cairo_image_surface_get_data(s);
        int stride = cairo_image_surface_get_stride(s);
        const unsigned char* src = static_cast<const unsigned char*>(data);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const unsigned char* p = src + (y * w + x) * 4;  // A R G B
                unsigned a = p[0];
                uint32_t r = p[1] * a / 255, g = p[2] * a / 255, b = p[3] * a / 255;
                *reinterpret_cast<uint32_t*>(dst + y * stride + x * 4) = (a << 24) | (r << 16) | (g << 8) | b;
            }
        cairo_surface_mark_dirty(s);
        best.reset(s, cairo_surface_destroy);
        best_w = w;
    }
    sd_bus_message_exit_container(m);
    return best;
}

// One (ia{sv}av) dbusmenu node, recursively.
bool read_menu_node(sd_bus_message* m, TrayMenuItem& out) {
    if (sd_bus_message_enter_container(m, 'r', "ia{sv}av") <= 0) return false;
    sd_bus_message_read(m, "i", &out.id);
    if (sd_bus_message_enter_container(m, 'a', "{sv}") > 0) {
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            sd_bus_message_read(m, "s", &key);
            std::string k = key ? key : "";
            char type = 0;
            const char* contents = nullptr;
            sd_bus_message_peek_type(m, &type, &contents);
            if (sd_bus_message_enter_container(m, 'v', contents) > 0) {
                char t = contents ? contents[0] : 0;
                if (t == 's') {
                    const char* v = nullptr;
                    sd_bus_message_read(m, "s", &v);
                    std::string sv = v ? v : "";
                    if (k == "label") {
                        // mnemonics: "_File" -> "File" ("__" is a literal underscore)
                        std::string lab;
                        for (size_t i = 0; i < sv.size(); ++i) {
                            if (sv[i] == '_' && i + 1 < sv.size() && sv[i + 1] == '_') {
                                lab += '_';
                                ++i;
                            } else if (sv[i] != '_') {
                                lab += sv[i];
                            }
                        }
                        out.label = lab;
                    } else if (k == "type") out.separator = sv == "separator";
                    else if (k == "toggle-type") out.toggle = sv == "checkmark" ? 1 : sv == "radio" ? 2 : 0;
                } else if (t == 'b') {
                    int v = 0;
                    sd_bus_message_read(m, "b", &v);
                    if (k == "enabled") out.enabled = v;
                    else if (k == "visible") out.visible = v;
                } else if (t == 'i') {
                    int v = 0;
                    sd_bus_message_read(m, "i", &v);
                    if (k == "toggle-state") out.checked = v == 1;
                } else {
                    sd_bus_message_skip(m, contents);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    if (sd_bus_message_enter_container(m, 'a', "v") > 0) {
        while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
            TrayMenuItem child;
            if (read_menu_node(m, child)) out.children.push_back(std::move(child));
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);
    return true;
}

}  // namespace

Tray::~Tray() {
    if (vtable_slot_) sd_bus_slot_unref(vtable_slot_);
}

bool Tray::init(sd_bus* bus) {
    bus_ = bus;
    if (!bus_) return false;
    memcpy(g_vtable, kVtable, sizeof(kVtable));
    g_vtable[1].x.method.handler = m_register_item;
    g_vtable[2].x.method.handler = m_register_host;
    g_vtable[3].x.property.get = p_items;
    g_vtable[4].x.property.get = p_host;
    g_vtable[5].x.property.get = p_version;
    if (sd_bus_add_object_vtable(bus_, &vtable_slot_, kWatcherPath, kWatcher, g_vtable, this) < 0) return false;
    int r = sd_bus_request_name(bus_, kWatcher, 0);
    if (r < 0) std::cerr << "[tray] another StatusNotifierWatcher is running; not hosting the tray\n";
    std::string host = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
    sd_bus_request_name(bus_, host.c_str(), 0);
    sd_bus_match_signal(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner_changed, this);
    // icon / title / status changes from any item
    sd_bus_add_match(bus_, nullptr, "type='signal',interface='org.kde.StatusNotifierItem'", on_item_signal, this);
    emit("StatusNotifierHostRegistered", "");
    return r >= 0;
}

TrayItem* Tray::find(const std::string& key) {
    for (auto& it : items_)
        if (it.key == key) return &it;
    return nullptr;
}

void Tray::emit(const char* signal, const std::string& arg) {
    if (std::string(signal) == "StatusNotifierHostRegistered")
        sd_bus_emit_signal(bus_, kWatcherPath, kWatcher, signal, "");
    else sd_bus_emit_signal(bus_, kWatcherPath, kWatcher, signal, "s", arg.c_str());
}

int Tray::m_register_item(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Tray*>(ud);
    const char* arg = nullptr;
    sd_bus_message_read(m, "s", &arg);
    std::string a = arg ? arg : "";
    std::string sender = sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "";
    // Either a bus name (path is the default) or an object path on the sender.
    std::string service = a, path = "/StatusNotifierItem";
    if (!a.empty() && a[0] == '/') {
        service = sender;
        path = a;
    }
    self->add(service, path);
    return sd_bus_reply_method_return(m, "");
}

int Tray::m_register_host(sd_bus_message* m, void* ud, sd_bus_error*) {
    static_cast<Tray*>(ud)->emit("StatusNotifierHostRegistered", "");
    return sd_bus_reply_method_return(m, "");
}

int Tray::p_items(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void* ud, sd_bus_error*) {
    auto* self = static_cast<Tray*>(ud);
    sd_bus_message_open_container(reply, 'a', "s");
    for (auto& it : self->items_) sd_bus_message_append(reply, "s", (it.service + it.path).c_str());
    return sd_bus_message_close_container(reply);
}

int Tray::p_host(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "b", 1);
}

int Tray::p_version(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply, void*, sd_bus_error*) {
    return sd_bus_message_append(reply, "i", 0);
}

void Tray::add(const std::string& service, const std::string& path) {
    std::string key = service + path;
    if (!find(key)) {
        TrayItem it;
        it.key = key;
        it.service = service;
        it.path = path;
        items_.push_back(it);
        emit("StatusNotifierItemRegistered", key);
        sd_bus_emit_properties_changed(bus_, kWatcherPath, kWatcher, "RegisteredStatusNotifierItems", nullptr);
    }
    refresh(key);
}

void Tray::remove_service(const std::string& service) {
    bool changed = false;
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->service == service) {
            emit("StatusNotifierItemUnregistered", it->key);
            it = items_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed && on_change) on_change();
}

int Tray::on_owner_changed(sd_bus_message* m, void* ud, sd_bus_error*) {
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0) return 0;
    if (new_owner && *new_owner) return 0;  // only names that went away
    auto* self = static_cast<Tray*>(ud);
    self->remove_service(name ? name : "");
    if (old_owner && *old_owner) self->remove_service(old_owner);
    return 0;
}

int Tray::on_item_signal(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Tray*>(ud);
    std::string sender = sd_bus_message_get_sender(m) ? sd_bus_message_get_sender(m) : "";
    std::string path = sd_bus_message_get_path(m) ? sd_bus_message_get_path(m) : "";
    for (auto& it : self->items_) {
        // items registered by well-known name: match on the unique sender too
        if (it.path == path && (it.service == sender || it.service[0] != ':')) self->refresh(it.key);
    }
    return 0;
}

struct RefreshCtx {
    Tray* self;
    std::string key;
};

void Tray::refresh(const std::string& key) {
    TrayItem* it = find(key);
    if (!it) return;
    auto* ctx = new RefreshCtx{this, key};
    sd_bus_call_method_async(
        bus_, nullptr, it->service.c_str(), it->path.c_str(), "org.freedesktop.DBus.Properties", "GetAll",
        [](sd_bus_message* reply, void* ud, sd_bus_error*) -> int {
            std::unique_ptr<RefreshCtx> c(static_cast<RefreshCtx*>(ud));
            TrayItem* it = c->self->find(c->key);
            if (!it || sd_bus_message_is_method_error(reply, nullptr)) return 0;
            if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) return 0;
            while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                const char* k = nullptr;
                sd_bus_message_read(reply, "s", &k);
                std::string key = k ? k : "";
                const char* contents = nullptr;
                char type = 0;
                sd_bus_message_peek_type(reply, &type, &contents);
                std::string sig = contents ? contents : "";
                if (sd_bus_message_enter_container(reply, 'v', contents) > 0) {
                    if (sig == "s") {
                        const char* v = nullptr;
                        sd_bus_message_read(reply, "s", &v);
                        std::string sv = v ? v : "";
                        if (key == "Id") it->id = sv;
                        else if (key == "Title") it->title = sv;
                        else if (key == "Status") it->status = sv;
                        else if (key == "IconName") it->icon_name = sv;
                        else if (key == "IconThemePath") it->theme_path = sv;
                    } else if (sig == "o") {
                        const char* v = nullptr;
                        sd_bus_message_read(reply, "o", &v);
                        if (key == "Menu") it->menu_path = v ? v : "";
                    } else if (sig == "b") {
                        int v = 0;
                        sd_bus_message_read(reply, "b", &v);
                        if (key == "ItemIsMenu") it->item_is_menu = v;
                    } else if (sig == "a(iiay)" && key == "IconPixmap") {
                        it->pixmap = read_pixmap(reply);
                    } else {
                        sd_bus_message_skip(reply, contents);
                    }
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            if (c->self->on_change) c->self->on_change();
            return 0;
        },
        ctx, "s", kItem);
}

void Tray::activate(const std::string& key, int x, int y) {
    TrayItem* it = find(key);
    if (!it) return;
    sd_bus_call_method_async(bus_, nullptr, it->service.c_str(), it->path.c_str(), kItem, "Activate", nullptr, nullptr,
                             "ii", x, y);
}

void Tray::secondary(const std::string& key, int x, int y) {
    TrayItem* it = find(key);
    if (!it) return;
    sd_bus_call_method_async(bus_, nullptr, it->service.c_str(), it->path.c_str(), kItem, "SecondaryActivate", nullptr,
                             nullptr, "ii", x, y);
}

void Tray::scroll(const std::string& key, int delta) {
    TrayItem* it = find(key);
    if (!it) return;
    sd_bus_call_method_async(bus_, nullptr, it->service.c_str(), it->path.c_str(), kItem, "Scroll", nullptr, nullptr,
                             "is", delta, "vertical");
}

struct MenuCtx {
    std::function<void(std::vector<TrayMenuItem>)> done;
};

void Tray::menu(const std::string& key, std::function<void(std::vector<TrayMenuItem>)> done) {
    TrayItem* it = find(key);
    if (!it || it->menu_path.empty()) {
        done({});
        return;
    }
    // Let the app refresh it first (AboutToShow), then fetch the whole tree.
    sd_bus_call_method_async(bus_, nullptr, it->service.c_str(), it->menu_path.c_str(), kMenu, "AboutToShow", nullptr,
                             nullptr, "i", 0);
    auto* ctx = new MenuCtx{std::move(done)};
    int r = sd_bus_call_method_async(
        bus_, nullptr, it->service.c_str(), it->menu_path.c_str(), kMenu, "GetLayout",
        [](sd_bus_message* reply, void* ud, sd_bus_error*) -> int {
            std::unique_ptr<MenuCtx> c(static_cast<MenuCtx*>(ud));
            std::vector<TrayMenuItem> out;
            if (!sd_bus_message_is_method_error(reply, nullptr)) {
                uint32_t rev = 0;
                sd_bus_message_read(reply, "u", &rev);
                TrayMenuItem root;
                if (read_menu_node(reply, root)) out = std::move(root.children);
            }
            c->done(std::move(out));
            return 0;
        },
        ctx, "iias", 0, -1, 0);
    if (r < 0) {
        std::unique_ptr<MenuCtx> c(ctx);
        c->done({});
    }
}

void Tray::menu_click(const std::string& key, int id) {
    TrayItem* it = find(key);
    if (!it || it->menu_path.empty()) return;
    sd_bus_message* m = nullptr;
    if (sd_bus_message_new_method_call(bus_, &m, it->service.c_str(), it->menu_path.c_str(), kMenu, "Event") < 0) return;
    sd_bus_message_append(m, "is", id, "clicked");
    sd_bus_message_open_container(m, 'v', "i");
    sd_bus_message_append(m, "i", 0);
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "u", static_cast<uint32_t>(now_epoch()));
    sd_bus_call_async(bus_, nullptr, m, nullptr, nullptr, 0);
    sd_bus_message_unref(m);
}

}  // namespace island
