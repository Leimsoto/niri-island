#pragma once

#include <string>
#include <vector>

namespace island::workshop {

// Wallpaper Engine's Steam Workshop on disk: the items Steam downloaded,
// read from their project.json, kept to what the accounts here are still
// subscribed to. Blocking (file system): fine from a worker.

// Steam libraries (folders holding steamapps), from libraryfolders.vdf.
std::vector<std::string> steam_libraries();
// Wallpaper Engine's Workshop folders in those libraries.
std::vector<std::string> workshop_dirs();

// A Workshop item, read from its project.json.
struct Item {
    std::string dir, title, preview, type;  // type: scene, video, web
};
bool read_item(const std::string& dir, Item& out);
bool is_item(const std::string& path);  // a folder holding a project.json
// Every downloaded item still subscribed to (and anything newer than Steam's
// list), your own projects, and the bundled ones Wallpaper Engine shows.
std::vector<Item> items();
// Deletes the Workshop downloads you are no longer subscribed to (Steam
// leaves them behind). Returns how many went.
int clean_workshop();

}  // namespace island::workshop
