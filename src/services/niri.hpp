#pragma once

#include <cstdint>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace island {

struct Workspace {
    uint64_t id = 0;
    int idx = 0;
    std::string name, output;
    bool active = false, focused = false, urgent = false;
    uint64_t active_window = 0;
};

struct Window {
    uint64_t id = 0;
    std::string title, app_id;
    uint64_t workspace = 0;
    bool focused = false, urgent = false;
    bool floating = false;
    float tile_w = 0, tile_h = 0;  // its size in the layout (logical px)
};

// niri's IPC: one long-lived EventStream socket for state, a short-lived
// socket per action. Reconnects by itself when niri restarts.
class Niri {
public:
    ~Niri();
    bool init();

    // Workspaces of the focused output, by index.
    std::vector<Workspace> output_workspaces() const;
    int active_slot() const;
    const std::map<uint64_t, Window>& windows() const { return windows_; }
    const Window* focused_window() const;
    bool overview_open() const { return overview_; }
    // A window (the one in focus of a workspace on screen) covers most of a
    // w×h output: fullscreen or maximized. For pausing video wallpapers.
    bool screen_covered(float w, float h) const;
    bool connected() const { return fd_ >= 0; }

    void focus_workspace(uint64_t id);
    void focus_workspace_relative(int delta);
    void focus_window(uint64_t id);
    void action(const std::string& json_action);  // raw {"Action":{...}}
    // Focuses a window whose app id or title matches, else returns false.
    bool focus_app(const std::string& hint);

    std::function<void()> on_change;

private:
    std::string path_;
    int fd_ = -1;
    std::string buf_;
    std::map<uint64_t, Workspace> workspaces_;
    std::map<uint64_t, Window> windows_;
    bool overview_ = false;
    uint64_t retry_timer_ = 0;
    double backoff_ = 1.0;

    void connect_stream();
    void disconnect();
    bool readable();
    void handle(const std::string& line);
};

}  // namespace island
