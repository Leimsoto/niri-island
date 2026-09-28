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
        const char* sender = sd_bus_message_get_sender(m);
        if (sender) {
            self->query_player_properties(sender);
        }
    }
    return 0;
}

int MprisClient::on_name_owner_changed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* self = static_cast<MprisClient*>(userdata);
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    int r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
    if (r >= 0 && strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
        self->query_active_player();
    }
    return 0;
}

bool MprisClient::init() {
    int r = sd_bus_default_user(&bus_);
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

    query_active_player();
    return true;
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
    std::string found_player;

    while (sd_bus_message_read(reply, "s", &name) > 0) {
        if (strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
            found_player = name;
            // Prefer playing player
            query_player_properties(name);
            if (info_.playing) {
                break;
            }
        }
    }

    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    if (found_player.empty()) {
        info_.active = false;
        info_.playing = false;
        info_.title.clear();
        info_.artist.clear();
        if (on_change) on_change(info_);
    }
}

void MprisClient::query_player_properties(const char* service) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    char* status = nullptr;

    int r = sd_bus_get_property_string(
        bus_, service, "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Player", "PlaybackStatus",
        &error, &status
    );

    if (r < 0) {
        sd_bus_error_free(&error);
        return;
    }

    current_player_service_ = service;
    info_.active = true;
    info_.playing = (status && strcmp(status, "Playing") == 0);
    info_.title.clear();
    info_.artist.clear();
    info_.art_url.clear();
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
                                if (t) info_.title = t;
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "xesam:artist") == 0) {
                            if (sd_bus_message_enter_container(reply, 'v', "as") >= 0) {
                                if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
                                    const char* a = nullptr;
                                    while (sd_bus_message_read(reply, "s", &a) > 0) {
                                        if (!info_.artist.empty()) info_.artist += ", ";
                                        info_.artist += a;
                                    }
                                    sd_bus_message_exit_container(reply);
                                }
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "mpris:artUrl") == 0) {
                            const char* u = nullptr;
                            if (sd_bus_message_enter_container(reply, 'v', "s") >= 0) {
                                sd_bus_message_read(reply, "s", &u);
                                if (u) {
                                    info_.art_url = u;
                                    std::cout << "[Island] Found artUrl: " << u << "\n";
                                }
                                sd_bus_message_exit_container(reply);
                            } else {
                                sd_bus_message_skip(reply, "v");
                            }
                        } else if (strcmp(key, "mpris:length") == 0) {
                            int64_t len = 0;
                            if (sd_bus_message_enter_container(reply, 'v', "x") >= 0) {
                                sd_bus_message_read(reply, "x", &len);
                                info_.length_us = len;
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
                                if (tid) info_.track_id = tid;
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

    query_position();

    if (on_change) {
        on_change(info_);
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
