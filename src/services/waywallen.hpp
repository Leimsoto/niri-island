#pragma once

#include <string>
#include <vector>

namespace island::waywallen {

// Wallpaper Engine through waywallen (github.com/waywallen/waywallen): its
// daemon renders scene, web and video wallpapers on a layer-shell surface;
// we drive it through its D-Bus name and its control WebSocket (protobuf,
// control.v1). The daemon only runs while a Wallpaper Engine wallpaper is
// on screen.
//
// Everything here blocks (sockets, processes): call it from a worker.

bool installed();      // the Flatpak (org.waywallen.waywallen) or a `waywallen` binary
bool plugin_installed();  // the open-wallpaper-engine plugin
bool running();        // its D-Bus name has an owner
bool display_installed();  // waywallen-layer-shell, which shows its frames (Flatpak cannot)
// Starts the daemon (no UI, no tray) if needed and waits for its control port.
bool ensure_running(int timeout_ms = 15000);
// Stops what it shows and quits it.
void stop_and_quit();
void set_paused(bool paused);
void set_muted(bool muted);
// Its own window (library, Workshop browser, plugins).
void open_ui();

struct Entry {
    std::string id, name, type, resource, preview;
    bool hidden = false;  // in waywallen's own library window
};
bool list(std::vector<Entry>& out, std::string* error = nullptr);
bool scan(std::string* error = nullptr);
bool set_hidden(const std::vector<std::string>& ids, bool hidden);
// waywallen's library shows what the picker shows: the rest is hidden there.
void sync_hidden();
bool apply(const std::string& id, std::string* error = nullptr);
// The wallpaper whose files are in `dir` (a Workshop item), rescanning once if it is new.
bool apply_dir(const std::string& dir, std::string* error = nullptr);

// One-time setup: installs the plugin from `zip` and registers the Steam
// library (the folder holding steamapps).
bool install_plugin(const std::string& zip, std::string* error = nullptr);
bool add_library(const std::string& steam_root, std::string* error = nullptr);

// Steam libraries (folders holding steamapps), from libraryfolders.vdf.
std::vector<std::string> steam_libraries();
// Wallpaper Engine's Workshop folders in those libraries.
std::vector<std::string> workshop_dirs();

// A Workshop item, read from its project.json (no daemon needed).
struct Item {
    std::string dir, title, preview, type;  // type: scene, video, web
};
bool read_item(const std::string& dir, Item& out);
bool is_item(const std::string& path);  // a folder holding a project.json
std::vector<Item> items();             // every downloaded item
// Deletes the Workshop downloads you are no longer subscribed to (Steam
// leaves them behind). Returns how many went.
int clean_workshop();

}  // namespace island::waywallen
