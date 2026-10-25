#pragma once

#include <nlohmann/json.hpp>
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
// What an item lets you change (project.json general.properties): sliders,
// switches, choices. Text-only entries and Wallpaper Engine's own UI keys
// are left out. In the order the author gave them.
struct Property {
    std::string name, text, type;  // type: slider, bool, combo, color
    nlohmann::json value;          // its default
    double min = 0, max = 1, step = 0;
    std::vector<std::pair<std::string, nlohmann::json>> options;  // combo: label, value
    std::string condition;         // e.g. "followclock.value == false"
};
std::vector<Property> properties(const std::string& dir);
// Your values for an item (name → value, as the property's own type).
nlohmann::json user_values(const std::string& dir);
void set_user_value(const std::string& dir, const std::string& name, const nlohmann::json& value);
// The value in effect: yours, else the default.
nlohmann::json value_of(const Property& p, const nlohmann::json& user);
// Whether a property's condition holds with these values (unknown forms do).
bool visible(const Property& p, const std::vector<Property>& all, const nlohmann::json& user);
// Your values as the engine takes them (--properties): numbers only, a
// switch as 1/0, a choice among words as its index. Empty: none set.
std::string engine_overrides(const std::string& dir);

// Deletes the Workshop downloads you are no longer subscribed to (Steam
// leaves them behind). Returns how many went.
int clean_workshop();

}  // namespace island::workshop
