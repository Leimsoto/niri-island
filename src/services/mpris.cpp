#include "services/mpris.hpp"

#include <cstring>
#include <iostream>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

static const char* kPath = "/org/mpris/MediaPlayer2";
static const char* kPlayer = "org.mpris.MediaPlayer2.Player";
static const char* kRoot = "org.mpris.MediaPlayer2";

namespace {
// Carries the player name through an async callback.
struct Ctx {
    Mpris* self;
    std::string name;
};
}  // namespace

bool Mpris::init(sd_bus* bus) {
    bus_ = bus;
    if (!bus_) return false;
    sd_bus_match_signal(bus_, nullptr, nullptr, kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        on_props, this);
    sd_bus_match_signal(bus_, nullptr, nullptr, kPath, kPlayer, "Seeked", on_seeked, this);
    sd_bus_match_signal(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "NameOwnerChanged", on_owner, this);

    sd_bus_call_method_async(
        bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames",
        [](sd_bus_message* m, void* ud, sd_bus_error*) -> int {
            auto* self = static_cast<Mpris*>(ud);
            if (sd_bus_message_is_method_error(m, nullptr)) return 0;
            if (sd_bus_message_enter_container(m, 'a', "s") < 0) return 0;
            const char* n = nullptr;
            while (sd_bus_message_read_basic(m, 's', &n) > 0) {
                if (n && starts_with(n, "org.mpris.MediaPlayer2.") && !strstr(n, "playerctld")) {
                    std::string name = n;
                    // Resolve the unique owner: signals come from it, not the well-known name.
                    auto* ctx = new Ctx{self, name};
                    sd_bus_call_method_async(
                        self->bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                        "GetNameOwner",
                        [](sd_bus_message* r, void* u, sd_bus_error*) -> int {
                            auto* c = static_cast<Ctx*>(u);
                            const char* owner = nullptr;
                            if (!sd_bus_message_is_method_error(r, nullptr) &&
                                sd_bus_message_read_basic(r, 's', &owner) > 0 && owner)
                                c->self->add_player(c->name, owner);
                            delete c;
                            return 0;
                        },
                        ctx, "s", name.c_str());
                }
            }
            return 0;
        },
        this, "");
    return true;
}

Mpris::Player* Mpris::by_owner(const char* owner) {
    if (!owner) return nullptr;
    for (auto& [n, p] : players_)
        if (p.owner == owner || p.name == owner) return &p;
    return nullptr;
}

Mpris::Player* Mpris::cur() {
    auto it = players_.find(current_);
    return it == players_.end() ? nullptr : &it->second;
}

void Mpris::add_player(const std::string& name, const std::string& owner) {
    Player& p = players_[name];
    p.name = name;
    p.owner = owner;
    fetch_all(name);
}

void Mpris::remove_player(const std::string& name) {
    players_.erase(name);
    select();
}

void Mpris::fetch_all(const std::string& name) {
    auto* ctx = new Ctx{this, name};
    sd_bus_call_method_async(
        bus_, nullptr, name.c_str(), kPath, "org.freedesktop.DBus.Properties", "GetAll",
        [](sd_bus_message* m, void* u, sd_bus_error*) -> int {
            auto* c = static_cast<Ctx*>(u);
            auto it = c->self->players_.find(c->name);
            if (it != c->self->players_.end() && !sd_bus_message_is_method_error(m, nullptr)) {
                Dict props;
                if (read_dict(m, props) >= 0) {
                    it->second.position_at = now_mono();
                    c->self->apply(it->second, props);
                }
            }
            delete c;
            return 0;
        },
        ctx, "s", kPlayer);
    // Identity and desktop entry live on the root interface.
    auto* ctx2 = new Ctx{this, name};
    sd_bus_call_method_async(
        bus_, nullptr, name.c_str(), kPath, "org.freedesktop.DBus.Properties", "GetAll",
        [](sd_bus_message* m, void* u, sd_bus_error*) -> int {
            auto* c = static_cast<Ctx*>(u);
            auto it = c->self->players_.find(c->name);
            if (it != c->self->players_.end() && !sd_bus_message_is_method_error(m, nullptr)) {
                Dict props;
                if (read_dict(m, props) >= 0) {
                    if (auto v = find(props, "Identity")) it->second.identity = v->str();
                    if (auto v = find(props, "DesktopEntry")) it->second.desktop = v->str();
                    c->self->select();
                }
            }
            delete c;
            return 0;
        },
        ctx2, "s", kRoot);
}

void Mpris::fetch_position(const std::string& name) {
    auto* ctx = new Ctx{this, name};
    sd_bus_call_method_async(
        bus_, nullptr, name.c_str(), kPath, "org.freedesktop.DBus.Properties", "Get",
        [](sd_bus_message* m, void* u, sd_bus_error*) -> int {
            auto* c = static_cast<Ctx*>(u);
            auto it = c->self->players_.find(c->name);
            if (it != c->self->players_.end() && !sd_bus_message_is_method_error(m, nullptr) &&
                now_mono() > c->self->hold_until_) {
                Value v;
                if (read_variant(m, v) >= 0 && v.kind == Value::Kind::Int) {
                    it->second.position = v.i;
                    it->second.position_at = now_mono();
                }
            }
            delete c;
            return 0;
        },
        ctx, "ss", kPlayer, "Position");
}

void Mpris::apply(Player& p, const Dict& props) {
    bool status_changed = false;
    if (auto v = find(props, "PlaybackStatus")) {
        std::string s = v->str(p.status);
        if (s != p.status) {
            // Freeze the extrapolated position at the moment of change.
            if (p.status == "Playing") {
                p.position += static_cast<int64_t>((now_mono() - p.position_at) * p.rate * 1e6);
                p.position_at = now_mono();
            }
            p.status = s;
            status_changed = true;
        }
        if (s == "Playing") p.last_playing = now_mono();
    }
    if (auto v = find(props, "Metadata"); v && v->dict) {
        std::string old_track = p.metadata.count("mpris:trackid") ? p.metadata["mpris:trackid"].str() : "";
        std::string old_title = p.metadata.count("xesam:title") ? p.metadata["xesam:title"].str() : "";
        p.metadata = *v->dict;
        std::string nt = p.metadata.count("mpris:trackid") ? p.metadata["mpris:trackid"].str() : "";
        std::string ntitle = p.metadata.count("xesam:title") ? p.metadata["xesam:title"].str() : "";
        if (nt != old_track || ntitle != old_title) {
            p.position = 0;
            p.position_at = now_mono();
            // Many players don't announce the new position; ask.
            fetch_position(p.name);
        }
    }
    if (auto v = find(props, "Position")) {
        p.position = v->integer(p.position);
        p.position_at = now_mono();
    }
    if (auto v = find(props, "Rate")) p.rate = std::max(0.0, v->number(1.0));
    if (auto v = find(props, "CanGoNext")) p.can_next = v->boolean(true);
    if (auto v = find(props, "CanGoPrevious")) p.can_prev = v->boolean(true);
    if (auto v = find(props, "CanSeek")) p.can_seek = v->boolean(true);
    if (auto v = find(props, "Shuffle")) {
        p.has_shuffle = true;
        p.shuffle = v->boolean(false);
    }
    if (auto v = find(props, "LoopStatus")) {
        p.has_loop = true;
        p.loop = v->str("None");
    }
    if (status_changed) fetch_position(p.name);
    select();
}

void Mpris::select() {
    // A playing player wins (the most recently started); otherwise keep the
    // current one while it still has something loaded, else the last that played.
    std::string best;
    double best_t = -1;
    for (auto& [n, p] : players_) {
        if (p.status == "Playing" && p.last_playing > best_t) {
            best = n;
            best_t = p.last_playing;
        }
    }
    auto has_track = [](const Player& p) {
        auto it = p.metadata.find("xesam:title");
        return p.status != "Stopped" && it != p.metadata.end() && !it->second.str().empty();
    };
    if (best.empty()) {
        auto it = players_.find(current_);
        if (it != players_.end() && has_track(it->second)) {
            best = current_;
        } else {
            for (auto& [n, p] : players_) {
                if (has_track(p) && p.last_playing >= best_t) {
                    best = n;
                    best_t = p.last_playing;
                }
            }
        }
    }
    current_ = best;

    MediaInfo next;
    if (Player* p = cur(); p && has_track(*p)) {
        auto md = [&](const char* k) {
            auto it = p->metadata.find(k);
            return it == p->metadata.end() ? std::string() : it->second.joined();
        };
        next.active = true;
        next.playing = p->status == "Playing";
        next.player = p->name;
        next.identity = p->identity.empty() ? p->name.substr(23) : p->identity;
        next.desktop = p->desktop;
        next.title = md("xesam:title");
        next.artist = md("xesam:artist");
        next.album = md("xesam:album");
        next.art_url = md("mpris:artUrl");
        next.track_id = md("mpris:trackid");
        auto it = p->metadata.find("mpris:length");
        next.length_us = it == p->metadata.end() ? 0 : it->second.integer(0);
        next.can_next = p->can_next;
        next.can_prev = p->can_prev;
        next.can_seek = p->can_seek && next.length_us > 0;
        next.shuffle = p->shuffle;
        next.loop = p->loop;
        next.has_shuffle = p->has_shuffle;
        next.has_loop = p->has_loop;
    }
    bool changed = next.active != info_.active || next.playing != info_.playing || next.title != info_.title ||
                   next.artist != info_.artist || next.art_url != info_.art_url || next.length_us != info_.length_us ||
                   next.player != info_.player || next.shuffle != info_.shuffle || next.loop != info_.loop;
    bool new_track = next.active && (next.title != info_.title || next.player != info_.player);
    info_ = std::move(next);
    if (changed && on_change) on_change();
    if (new_track && info_.playing && on_track) on_track();
}

int64_t Mpris::position() const {
    if (scrub_ >= 0) return static_cast<int64_t>(scrub_ * info_.length_us);
    auto it = players_.find(current_);
    if (it == players_.end()) return 0;
    const Player& p = it->second;
    int64_t pos = p.position;
    if (p.status == "Playing") pos += static_cast<int64_t>((now_mono() - p.position_at) * p.rate * 1e6);
    if (info_.length_us > 0) pos = std::clamp<int64_t>(pos, 0, info_.length_us);
    return std::max<int64_t>(0, pos);
}

float Mpris::progress() const {
    if (info_.length_us <= 0) return 0;
    return clamp01(static_cast<float>(position()) / static_cast<float>(info_.length_us));
}

void Mpris::call(const char* method) {
    if (current_.empty()) return;
    sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, kPlayer, method, nullptr, nullptr, "");
}

void Mpris::play_pause() { call("PlayPause"); }
void Mpris::next() { call("Next"); }
void Mpris::previous() { call("Previous"); }
void Mpris::raise() {
    if (!current_.empty())
        sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, kRoot, "Raise", nullptr, nullptr, "");
}

void Mpris::seek_ratio(float r) {
    Player* p = cur();
    if (!p || info_.length_us <= 0) return;
    int64_t target = static_cast<int64_t>(clamp01(r) * info_.length_us);
    int64_t from = position();
    p->position = target;
    p->position_at = now_mono();
    hold_until_ = now_mono() + 0.8;
    if (!info_.track_id.empty() && info_.track_id != "/org/mpris/MediaPlayer2/TrackList/NoTrack") {
        sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, kPlayer, "SetPosition", nullptr, nullptr,
                                 "ox", info_.track_id.c_str(), target);
    } else {
        sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, kPlayer, "Seek", nullptr, nullptr, "x",
                                 target - from);
    }
}

void Mpris::set_scrub(float ratio) { scrub_ = clamp01(ratio); }

void Mpris::end_scrub(float ratio) {
    scrub_ = -1;
    seek_ratio(ratio);
}

void Mpris::toggle_shuffle() {
    Player* p = cur();
    if (!p || !p->has_shuffle) return;
    sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, "org.freedesktop.DBus.Properties", "Set", nullptr,
                             nullptr, "ssv", kPlayer, "Shuffle", "b", p->shuffle ? 0 : 1);
}

void Mpris::cycle_loop() {
    Player* p = cur();
    if (!p || !p->has_loop) return;
    const char* next = p->loop == "None" ? "Playlist" : p->loop == "Playlist" ? "Track" : "None";
    sd_bus_call_method_async(bus_, nullptr, current_.c_str(), kPath, "org.freedesktop.DBus.Properties", "Set", nullptr,
                             nullptr, "ssv", kPlayer, "LoopStatus", "s", next);
}

int Mpris::on_props(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Mpris*>(ud);
    const char* iface = nullptr;
    if (sd_bus_message_read_basic(m, 's', &iface) < 0 || !iface) return 0;
    Player* p = self->by_owner(sd_bus_message_get_sender(m));
    if (!p) return 0;
    if (strcmp(iface, kPlayer) == 0) {
        Dict props;
        if (read_dict(m, props) < 0) return 0;
        // Invalidated properties: refetch everything.
        std::vector<std::string> invalid;
        if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
            const char* s;
            while (sd_bus_message_read_basic(m, 's', &s) > 0) invalid.emplace_back(s);
            sd_bus_message_exit_container(m);
        }
        self->apply(*p, props);
        if (!invalid.empty()) self->fetch_all(p->name);
    } else if (strcmp(iface, kRoot) == 0) {
        Dict props;
        if (read_dict(m, props) < 0) return 0;
        if (auto v = find(props, "Identity")) p->identity = v->str();
    }
    return 0;
}

int Mpris::on_seeked(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Mpris*>(ud);
    Player* p = self->by_owner(sd_bus_message_get_sender(m));
    int64_t pos = 0;
    if (p && sd_bus_message_read_basic(m, 'x', &pos) > 0) {
        p->position = pos;
        p->position_at = now_mono();
        if (self->on_change) self->on_change();
    }
    return 0;
}

int Mpris::on_owner(sd_bus_message* m, void* ud, sd_bus_error*) {
    auto* self = static_cast<Mpris*>(ud);
    const char *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0 || !name) return 0;
    if (!starts_with(name, "org.mpris.MediaPlayer2.") || strstr(name, "playerctld")) return 0;
    if (new_owner && *new_owner) self->add_player(name, new_owner);
    else self->remove_player(name);
    return 0;
}

}  // namespace island
