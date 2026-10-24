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
bool apply(const std::string& path, Kind kind, bool muted, std::string* error = nullptr);
void stop();  // nothing shown any more; the server exits
void set_paused(bool paused);
void set_muted(bool muted);

// Kinds of Wallpaper Engine project it renders (the rest: web, for now elsewhere).
bool renders(const std::string& project_type);

}  // namespace island::engine
