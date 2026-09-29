#pragma once

#include <string>
#include <vector>
#include <functional>
#include <systemd/sd-bus.h>

namespace island {

struct MediaInfo {
    bool active = false;
    bool playing = false;
    std::string player_name;
    std::string title;
    std::string artist;
    std::string album;
    std::string art_url;
    std::string track_id = "/";
    int64_t position_us = 0;
    int64_t length_us = 0;
};

class MprisClient {
public:
    MprisClient();
    ~MprisClient();

    bool init();
    void update();
    int get_fd() const;

    void play_pause();
    void next();
    void previous();
    void set_position_ratio(float ratio);
    void query_position();
    void tick(float dt);

    const MediaInfo& info() const { return info_; }
    std::function<void(const MediaInfo&)> on_change;

private:
    sd_bus* bus_ = nullptr;
    MediaInfo info_;
    std::string current_player_service_;

    void query_active_player();
    MediaInfo fetch_player_info(const char* service);
    void query_player_properties(const char* service);
    static int on_properties_changed(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int on_name_owner_changed(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
};

} // namespace island
