#pragma once

#include <functional>
#include <map>
#include <string>
#include <systemd/sd-bus.h>

#include "services/dbus.hpp"

namespace island {

struct MediaInfo {
    bool active = false;
    bool playing = false;
    std::string player;      // bus name
    std::string identity;    // "Spotify"
    std::string desktop;     // desktop entry, for focusing the app
    std::string title, artist, album, art_url, track_id;
    int64_t length_us = 0;
    bool can_next = true, can_prev = true, can_seek = true;
    bool shuffle = false;
    std::string loop = "None";   // None | Track | Playlist
    bool has_shuffle = false, has_loop = false;
};

class Mpris {
public:
    bool init(sd_bus* bus);

    const MediaInfo& info() const { return info_; }
    // Extrapolated playback position (µs).
    int64_t position() const;
    float progress() const;

    void play_pause();
    void next();
    void previous();
    void seek_ratio(float r);
    void toggle_shuffle();
    void cycle_loop();
    void raise();

    // While the user drags the seek bar the displayed position is theirs.
    void set_scrub(float ratio);
    void end_scrub(float ratio);
    bool scrubbing() const { return scrub_ >= 0; }

    std::function<void()> on_change;      // track / status changed
    std::function<void()> on_track;       // a new track started (for the "now playing" toast)

private:
    struct Player {
        std::string name, owner, identity, desktop;
        std::string status = "Stopped";
        Dict metadata;
        int64_t position = 0;
        double position_at = 0;  // monotonic time position was sampled
        double rate = 1.0;
        double last_playing = 0;
        bool can_next = true, can_prev = true, can_seek = true;
        bool has_shuffle = false, has_loop = false;
        bool shuffle = false;
        std::string loop = "None";
    };

    sd_bus* bus_ = nullptr;
    std::map<std::string, Player> players_;  // by well-known name
    std::string current_;
    MediaInfo info_;
    float scrub_ = -1.0f;
    double hold_until_ = 0;  // ignore stale positions right after a seek

    Player* by_owner(const char* owner);
    Player* cur();
    void add_player(const std::string& name, const std::string& owner);
    void remove_player(const std::string& name);
    void fetch_all(const std::string& name);
    void fetch_position(const std::string& name);
    void apply(Player& p, const Dict& props);
    void select();
    void call(const char* method);

    static int on_props(sd_bus_message* m, void* ud, sd_bus_error*);
    static int on_seeked(sd_bus_message* m, void* ud, sd_bus_error*);
    static int on_owner(sd_bus_message* m, void* ud, sd_bus_error*);
};

}  // namespace island
