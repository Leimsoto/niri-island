#include "mpris.hpp"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace island {

MprisClient::MprisClient() = default;

MprisClient::~MprisClient() {
    if (bus_) {
        sd_bus_flush_close_unref(bus_);
    }
}

int MprisClient::on_properties_changed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<MprisClient*>(userdata);
    const char* interface = nullptr;
    int r = sd_bus_message_read(m, "s", &interface);
    if (r >= 0 && strcmp(interface, "org.mpris.MediaPlayer2.Player") == 0) {
        self->query_active_player();
    }
    return 1;
}

int MprisClient::on_name_owner_changed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<MprisClient*>(userdata);
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    int r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
    if (r >= 0 && strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
        self->query_active_player();
    }
    return 1;
}

bool MprisClient::init() {
    int r = sd_bus_open_user(&bus_);
    if (r < 0) {
        std::cerr << "Failed to connect to user D-Bus: " << strerror(-r) << "\n";
        return false;
    }

    sd_bus_match_signal(
        bus_, nullptr, nullptr, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        on_properties_changed, this
    );

    sd_bus_match_signal(
        bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameOwnerChanged",
        on_name_owner_changed, this
    );

    sd_bus_flush(bus_);

    query_active_player();
    return true;
}

MediaInfo MprisClient::fetch_player_info(const char* service) {
    MediaInfo info;
    if (!bus_ || !service) return info;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    char* status = nullptr;

    int r = sd_bus_get_property_string(
        bus_, service, "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "PlaybackStatus",
        &error, &status
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return info;
    }

    info.player_name = service;
    info.playing = (status && strcmp(status, "Playing") == 0);
    bool is_stopped = (status && strcmp(status, "Stopped") == 0);
    free(status);

    // Query Metadata
    sd_bus_message* reply = nullptr;
    r = sd_bus_call_method(
        bus_, service, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "Get", &error, &reply,
        "ss", "org.mpris.MediaPlayer2.Player", "Metadata"
    );

    if (r >= 0) {
        r = sd_bus_message_enter_container(reply, 'v', "a{sv}");
        if (r >= 0) {
            r = sd_bus_message_enter_container(reply, 'a', "{sv}");
            if (r >= 0) {
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char* key = nullptr;
                    sd_bus_message_read(reply, "s", &key);

                    if (key) {
                        if (strcmp(key, "xesam:title") == 0) {
                            const char* t = nullptr;
                            if (sd_bus_message_enter_container(reply, 'v', "s") >= 0) {
                                sd_bus_message_read(reply, "s", &t);
                                if (t) info.title = t;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "xesam:artist") == 0) {
                            if (sd_bus_message_enter_container(reply, 'v', nullptr) >= 0) {
                                if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
                                    const char* a = nullptr;
                                    while (sd_bus_message_read(reply, "s", &a) > 0) {
                                        if (!info.artist.empty()) info.artist += ", ";
                                        if (a) info.artist += a;
                                    }
                                    sd_bus_message_exit_container(reply);
                                } else {
                                    const char* a = nullptr;
                                    if (sd_bus_message_read(reply, "s", &a) > 0 && a) {
                                        info.artist = a;
                                    }
                                }
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "xesam:album") == 0) {
                            const char* alb = nullptr;
                            if (sd_bus_message_enter_container(reply, 'v', "s") >= 0) {
                                sd_bus_message_read(reply, "s", &alb);
                                if (alb) info.album = alb;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "mpris:artUrl") == 0) {
                            const char* u = nullptr;
                            if (sd_bus_message_enter_container(reply, 'v', "s") >= 0) {
                                sd_bus_message_read(reply, "s", &u);
                                if (u) info.art_url = u;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "mpris:length") == 0) {
                            int64_t len = 0;
                            if (sd_bus_message_enter_container(reply, 'v', "x") >= 0) {
                                sd_bus_message_read(reply, "x", &len);
                                info.length_us = len;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "mpris:trackid") == 0) {
                            const char* tid = nullptr;
                            if (sd_bus_message_enter_container(reply, 'v', nullptr) >= 0) {
                                if (sd_bus_message_read(reply, "o", &tid) < 0) {
                                    sd_bus_message_read(reply, "s", &tid);
                                }
                                if (tid) info.track_id = tid;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else {
                            sd_bus_message_skip(reply, "v");
                        }
                    }

                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&error);

    // Active if title is present and playback is not fully stopped
    info.active = !info.title.empty() && !is_stopped;
    return info;
}

void MprisClient::query_active_player() {
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_call_method(
        bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames", &error, &reply, ""
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return;
    }

    r = sd_bus_message_enter_container(reply, 'a', "s");
    if (r < 0) {
        sd_bus_message_unref(reply);
        return;
    }

    const char* name = nullptr;
    std::string best_service;
    MediaInfo best_info;

    while (sd_bus_message_read(reply, "s", &name) > 0) {
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
            if (strstr(name, "playerctld")) continue;

            MediaInfo cand = fetch_player_info(name);
            if (cand.active) {
                if (cand.playing) {
                    best_service = name;
                    best_info = cand;
                    break;
                } else if (best_service.empty() || !best_info.playing) {
                    best_service = name;
                    best_info = cand;
                }
            }
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    bool was_active = info_.active;
    bool was_playing = info_.playing;
    std::string prev_title = info_.title;
    std::string prev_art = info_.art_url;

    current_player_service_ = best_service;
    info_ = best_info;

    if (info_.active) {
        query_position();
    }

    bool changed = (was_active != info_.active ||
                    was_playing != info_.playing ||
                    prev_title != info_.title ||
                    prev_art != info_.art_url);

    if (changed && on_change) {
        on_change(info_);
    }
}

void MprisClient::query_player_properties(const char* service) {
    if (!service) return;
    MediaInfo cand = fetch_player_info(service);
    if (cand.active) {
        current_player_service_ = service;
        info_ = cand;
        query_position();
        if (on_change) on_change(info_);
    } else if (current_player_service_ == service) {
        query_active_player();
    }
}

void MprisClient::query_position() {
    if (!bus_ || current_player_service_.empty() || !info_.active) return;
    int64_t pos = 0;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_trivial(
        bus_, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "Position",
        &error, 'x', &pos
    );
    if (r >= 0) {
        info_.position_us = pos;
    }
    sd_bus_error_free(&error);
}

void MprisClient::set_position_ratio(float ratio) {
    if (!bus_ || current_player_service_.empty() || info_.length_us <= 0) return;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    int64_t prev_us = info_.position_us;
    int64_t target_us = static_cast<int64_t>(ratio * static_cast<float>(info_.length_us));
    info_.position_us = target_us;

    sd_bus_error error = SD_BUS_ERROR_NULL;
    const char* tid = info_.track_id.empty() ? "/" : info_.track_id.c_str();

    int r = sd_bus_call_method(
        bus_, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "SetPosition",
        &error, nullptr, "ox", tid, target_us
    );
    if (r < 0) {
        sd_bus_error_free(&error);
        int64_t offset_us = target_us - prev_us;
        sd_bus_call_method(
            bus_, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
            "org.mpris.MediaPlayer2.Player", "Seek",
            nullptr, nullptr, "x", offset_us
        );
    }
    sd_bus_error_free(&error);
}

void MprisClient::tick(float dt) {
    if (info_.active && info_.playing && info_.length_us > 0) {
        info_.position_us += static_cast<int64_t>(dt * 1000000.0f);
        if (info_.position_us > info_.length_us) {
            info_.position_us = info_.length_us;
        }
    }

    poll_timer_ += dt;
    if (poll_timer_ >= 2.0f) {
        poll_timer_ = 0.0f;
        query_active_player();
    }
}

void MprisClient::update() {
    if (bus_) {
        while (sd_bus_process(bus_, nullptr) > 0) {}
    }
}

int MprisClient::get_fd() const {
    return bus_ ? sd_bus_get_fd(bus_) : -1;
}

void MprisClient::play_pause() {
    if (current_player_service_.empty()) return;
    sd_bus_call_method_async(
        bus_, nullptr, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "PlayPause", nullptr, nullptr, ""
    );
}

void MprisClient::next() {
    if (current_player_service_.empty()) return;
    sd_bus_call_method_async(
        bus_, nullptr, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "Next", nullptr, nullptr, ""
    );
}

void MprisClient::previous() {
    if (current_player_service_.empty()) return;
    sd_bus_call_method_async(
        bus_, nullptr, current_player_service_.c_str(), "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "Previous", nullptr, nullptr, ""
    );
}

} // namespace island
