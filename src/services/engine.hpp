#pragma once

#include <string>

namespace island::engine {

// niri-island-engine (engine/, after skwd-paper): Wallpaper Engine scenes and
// videos, and plain videos, drawn with Vulkan on its own background surface.
// Driven through its command line, which talks to its server (started on the
// first apply, gone after stop).
//
// Everything here blocks: call it from a worker.

bool installed();  // niri-island-engine next to niri-island or on PATH
bool running();    // its server is up (showing something)

enum class Kind { Video, Engine };  // a video file, a Wallpaper Engine folder

// Shows `path` on every screen, replacing whatever it showed.
// `properties`: a Wallpaper Engine item's values (JSON, see workshop::engine_overrides).
bool apply(const std::string& path, Kind kind, bool muted, std::string* error = nullptr,
           const std::string& properties = "");
void stop();  // nothing shown any more; the server exits
// New values for the shown Wallpaper Engine scene's settings: live when it can.
bool set_properties(const std::string& properties);
void set_paused(bool paused);
void set_muted(bool muted);

// A still of a Wallpaper Engine folder or a video, rendered offscreen as a
// PNG (1280×720 at most); no server needed.
bool capture(const std::string& source, const std::string& png);

// Kinds of Wallpaper Engine project it renders (the rest: web, for now elsewhere).
bool renders(const std::string& project_type);

}  // namespace island::engine
