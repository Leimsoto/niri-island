#include "services/audio.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include "core/loop.hpp"
#include "core/util.hpp"

namespace island {

Audio::~Audio() { disconnect(); }

bool Audio::init() {
    ml_ = pa_threaded_mainloop_new();
    if (!ml_) return false;
    if (pa_threaded_mainloop_start(ml_) < 0) return false;
    connect();
    return true;
}

void Audio::with_lock(const std::function<void()>& f) {
    if (!ml_) return;
    pa_threaded_mainloop_lock(ml_);
    if (ctx_ && pa_context_get_state(ctx_) == PA_CONTEXT_READY) f();
    pa_threaded_mainloop_unlock(ml_);
}

void Audio::connect() {
    pa_threaded_mainloop_lock(ml_);
    pa_mainloop_api* api = pa_threaded_mainloop_get_api(ml_);
    pa_proplist* props = pa_proplist_new();
    pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "niri-island");
    pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "niri-island");
    ctx_ = pa_context_new_with_proplist(api, "niri-island", props);
    pa_proplist_free(props);
    pa_context_set_state_callback(ctx_, ctx_state, this);
    pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOFAIL, nullptr);
    pa_threaded_mainloop_unlock(ml_);
}

void Audio::disconnect() {
    if (!ml_) return;
    pa_threaded_mainloop_lock(ml_);
    if (ctx_) {
        pa_context_set_state_callback(ctx_, nullptr, nullptr);
        pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
        pa_context_disconnect(ctx_);
        pa_context_unref(ctx_);
        ctx_ = nullptr;
    }
    pa_threaded_mainloop_unlock(ml_);
    pa_threaded_mainloop_stop(ml_);
    pa_threaded_mainloop_free(ml_);
    ml_ = nullptr;
}

void Audio::ctx_state(pa_context* c, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY: {
            pa_context_set_subscribe_callback(c, ctx_event, self);
            auto mask = static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE |
                                                            PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                                            PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT |
                                                            PA_SUBSCRIPTION_MASK_SERVER);
            if (auto* op = pa_context_subscribe(c, mask, nullptr, nullptr)) pa_operation_unref(op);
            self->refresh_all();
            Loop::get().post([self] {
                // Values reported while starting up are not the user asking for an OSD.
                self->armed_at_ = now_mono() + 0.8;
            });
            break;
        }
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            // pipewire-pulse restarted: NOFAIL keeps retrying the connection,
            // but a terminated context needs a fresh one.
            Loop::get().post([self] {
                if (self->reconnect_timer_) return;
                self->st_.ready = false;
                self->reconnect_timer_ = Loop::get().after(2.0, [self] {
                    self->reconnect_timer_ = 0;
                    if (!self->ml_) return;
                    pa_threaded_mainloop_lock(self->ml_);
                    if (self->ctx_) {
                        pa_context_set_state_callback(self->ctx_, nullptr, nullptr);
                        pa_context_disconnect(self->ctx_);
                        pa_context_unref(self->ctx_);
                        self->ctx_ = nullptr;
                    }
                    pa_threaded_mainloop_unlock(self->ml_);
                    self->connect();
                });
            });
            break;
        default: break;
    }
}

void Audio::refresh_all() {
    // Called on the pulse thread with the lock held.
    if (auto* op = pa_context_get_server_info(ctx_, server_cb, this)) pa_operation_unref(op);
}

void Audio::ctx_event(pa_context* c, pa_subscription_event_type_t t, uint32_t, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    unsigned facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    pa_operation* op = nullptr;
    switch (facility) {
        case PA_SUBSCRIPTION_EVENT_SERVER: op = pa_context_get_server_info(c, server_cb, self); break;
        case PA_SUBSCRIPTION_EVENT_SINK: op = pa_context_get_sink_info_list(c, sinks_cb, self); break;
        case PA_SUBSCRIPTION_EVENT_SOURCE: op = pa_context_get_source_info_list(c, sources_cb, self); break;
        case PA_SUBSCRIPTION_EVENT_SINK_INPUT: op = pa_context_get_sink_input_info_list(c, inputs_cb, self); break;
        case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
            op = pa_context_get_source_output_info_list(c, outputs_cb, self);
            break;
        default: break;
    }
    if (op) pa_operation_unref(op);
}

void Audio::server_cb(pa_context* c, const pa_server_info* i, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    if (!i) return;
    std::string sink = i->default_sink_name ? i->default_sink_name : "";
    std::string source = i->default_source_name ? i->default_source_name : "";
    Loop::get().post([self, sink, source] {
        if (!self->default_sink_.empty() && sink != self->default_sink_) {
            // A device switch reports its own volume: not an OSD moment.
            self->suppress_until_ = now_mono() + 1.5;
        }
        self->default_sink_ = sink;
        self->default_source_ = source;
    });
    pa_operation* ops[] = {pa_context_get_sink_info_list(c, sinks_cb, self),
                           pa_context_get_source_info_list(c, sources_cb, self),
                           pa_context_get_sink_input_info_list(c, inputs_cb, self),
                           pa_context_get_source_output_info_list(c, outputs_cb, self)};
    for (auto* op : ops)
        if (op) pa_operation_unref(op);
}

static float cvol(const pa_cvolume& v) { return static_cast<float>(pa_cvolume_max(&v)) / PA_VOLUME_NORM; }

void Audio::sinks_cb(pa_context*, const pa_sink_info* i, int eol, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    if (eol == 0 && i) {
        AudioDevice d;
        d.index = i->index;
        d.name = i->name ? i->name : "";
        d.description = i->description ? i->description : d.name;
        const char* icon = pa_proplist_gets(i->proplist, PA_PROP_DEVICE_ICON_NAME);
        d.icon = icon ? icon : "";
        d.volume = cvol(i->volume);
        d.muted = i->mute != 0;
        d.channels = i->volume.channels;
        self->acc_sinks_.push_back(std::move(d));
        return;
    }
    auto list = std::move(self->acc_sinks_);
    self->acc_sinks_.clear();
    Loop::get().post([self, list = std::move(list)]() mutable {
        AudioState& st = self->st_;
        AudioDevice prev = st.sink;
        bool had = st.ready;
        st.sinks = std::move(list);
        for (auto& d : st.sinks) {
            d.is_default = d.name == self->default_sink_;
            if (d.is_default) st.sink = d;
        }
        st.ready = true;
        bool changed = std::abs(prev.volume - st.sink.volume) > 0.004f || prev.muted != st.sink.muted;
        bool user = had && changed && prev.name == st.sink.name && now_mono() > self->armed_at_ &&
                    now_mono() > self->suppress_until_ && self->armed_at_ > 0;
        if (changed && self->on_sink_change) self->on_sink_change(user);
        if (self->on_change) self->on_change();
    });
}

void Audio::sources_cb(pa_context*, const pa_source_info* i, int eol, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    if (eol == 0 && i) {
        if (i->monitor_of_sink != PA_INVALID_INDEX) {
            // Monitors are not microphones; remember them so captures of the
            // desktop audio (visualisers, screen recorders) are not "mic in use".
            if (std::find(self->monitor_sources_.begin(), self->monitor_sources_.end(), i->index) ==
                self->monitor_sources_.end())
                self->monitor_sources_.push_back(i->index);
            return;
        }
        AudioDevice d;
        d.index = i->index;
        d.name = i->name ? i->name : "";
        d.description = i->description ? i->description : d.name;
        d.volume = cvol(i->volume);
        d.muted = i->mute != 0;
        d.channels = i->volume.channels;
        self->acc_sources_.push_back(std::move(d));
        return;
    }
    auto list = std::move(self->acc_sources_);
    self->acc_sources_.clear();
    Loop::get().post([self, list = std::move(list)]() mutable {
        AudioState& st = self->st_;
        bool prev_muted = st.source.muted;
        st.sources = std::move(list);
        for (auto& d : st.sources) {
            d.is_default = d.name == self->default_source_;
            if (d.is_default) st.source = d;
        }
        if (prev_muted != st.source.muted && self->on_mic_mute && now_mono() > self->armed_at_ && self->armed_at_ > 0)
            self->on_mic_mute(st.source.muted);
        if (self->on_change) self->on_change();
    });
}

void Audio::inputs_cb(pa_context*, const pa_sink_input_info* i, int eol, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    if (eol == 0 && i) {
        AudioStream s;
        s.index = i->index;
        const char* app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
        const char* icon = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_ICON_NAME);
        const char* bin = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
        const char* media = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_NAME);
        s.app = app ? app : (bin ? bin : "App");
        s.icon = icon ? icon : (bin ? bin : "");
        s.media = media ? media : "";
        s.volume = cvol(i->volume);
        s.muted = i->mute != 0;
        s.channels = i->volume.channels;
        s.sink = i->sink;
        const char* role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
        if (role && strcmp(role, "event") == 0) return;  // short system sounds
        self->acc_streams_.push_back(std::move(s));
        return;
    }
    auto list = std::move(self->acc_streams_);
    self->acc_streams_.clear();
    Loop::get().post([self, list = std::move(list)]() mutable {
        self->st_.streams = std::move(list);
        if (self->on_change) self->on_change();
    });
}

void Audio::outputs_cb(pa_context*, const pa_source_output_info* i, int eol, void* ud) {
    auto* self = static_cast<Audio*>(ud);
    if (eol == 0 && i) {
        if (std::find(self->monitor_sources_.begin(), self->monitor_sources_.end(), i->source) !=
            self->monitor_sources_.end())
            return;
        const char* app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
        const char* bin = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
        std::string name = app ? app : (bin ? bin : "");
        // Level meters (pavucontrol, the shells' own visualisers) are not recording.
        const char* flags = pa_proplist_gets(i->proplist, "stream.capture.sink");
        std::string lname = lower(name);
        if (flags || lname.find("peak") != std::string::npos || lname == "niri-island" ||
            lname.find("pavucontrol") != std::string::npos || lname.find("cava") != std::string::npos ||
            lname.find("noctalia") != std::string::npos || lname.find("quickshell") != std::string::npos)
            return;
        if (!name.empty() && std::find(self->acc_mic_.begin(), self->acc_mic_.end(), name) == self->acc_mic_.end())
            self->acc_mic_.push_back(name);
        return;
    }
    auto list = std::move(self->acc_mic_);
    self->acc_mic_.clear();
    Loop::get().post([self, list = std::move(list)]() mutable {
        bool changed = list != self->st_.mic_apps;
        self->st_.mic_apps = std::move(list);
        if (changed && self->on_change) self->on_change();
    });
}

// ── control ────────────────────────────────────────────────────────────────

void Audio::set_volume(float v) {
    v = std::clamp(v, 0.0f, 1.5f);
    st_.sink.volume = v;
    std::string name = st_.sink.name;
    uint8_t ch = std::max<uint8_t>(1, st_.sink.channels);
    with_lock([&] {
        pa_cvolume cv;
        pa_cvolume_set(&cv, ch, static_cast<pa_volume_t>(std::lround(v * PA_VOLUME_NORM)));
        if (auto* op = pa_context_set_sink_volume_by_name(ctx_, name.empty() ? "@DEFAULT_SINK@" : name.c_str(), &cv,
                                                          nullptr, nullptr))
            pa_operation_unref(op);
        if (v > 0.001f && st_.sink.muted) {
            if (auto* op = pa_context_set_sink_mute_by_name(ctx_, "@DEFAULT_SINK@", 0, nullptr, nullptr))
                pa_operation_unref(op);
        }
    });
}

void Audio::change_volume(float delta) {
    float v = std::round((st_.sink.volume + delta) * 100.0f) / 100.0f;
    set_volume(std::clamp(v, 0.0f, 1.0f));
}

void Audio::toggle_mute() {
    bool m = !st_.sink.muted;
    st_.sink.muted = m;
    with_lock([&] {
        if (auto* op = pa_context_set_sink_mute_by_name(ctx_, "@DEFAULT_SINK@", m, nullptr, nullptr))
            pa_operation_unref(op);
    });
}

void Audio::set_mic_volume(float v) {
    v = std::clamp(v, 0.0f, 1.5f);
    st_.source.volume = v;
    uint8_t ch = std::max<uint8_t>(1, st_.source.channels);
    with_lock([&] {
        pa_cvolume cv;
        pa_cvolume_set(&cv, ch, static_cast<pa_volume_t>(std::lround(v * PA_VOLUME_NORM)));
        if (auto* op = pa_context_set_source_volume_by_name(ctx_, "@DEFAULT_SOURCE@", &cv, nullptr, nullptr))
            pa_operation_unref(op);
    });
}

void Audio::toggle_mic_mute() {
    bool m = !st_.source.muted;
    st_.source.muted = m;
    with_lock([&] {
        if (auto* op = pa_context_set_source_mute_by_name(ctx_, "@DEFAULT_SOURCE@", m, nullptr, nullptr))
            pa_operation_unref(op);
    });
}

void Audio::set_default_sink(const std::string& name) {
    suppress_until_ = now_mono() + 1.5;
    with_lock([&] {
        if (auto* op = pa_context_set_default_sink(ctx_, name.c_str(), nullptr, nullptr)) pa_operation_unref(op);
    });
}

void Audio::set_default_source(const std::string& name) {
    with_lock([&] {
        if (auto* op = pa_context_set_default_source(ctx_, name.c_str(), nullptr, nullptr)) pa_operation_unref(op);
    });
}

void Audio::set_stream_volume(uint32_t index, float v) {
    v = std::clamp(v, 0.0f, 1.5f);
    uint8_t ch = 2;
    for (auto& s : st_.streams)
        if (s.index == index) {
            s.volume = v;
            ch = std::max<uint8_t>(1, s.channels);
        }
    with_lock([&] {
        pa_cvolume cv;
        pa_cvolume_set(&cv, ch, static_cast<pa_volume_t>(std::lround(v * PA_VOLUME_NORM)));
        if (auto* op = pa_context_set_sink_input_volume(ctx_, index, &cv, nullptr, nullptr)) pa_operation_unref(op);
    });
}

void Audio::toggle_stream_mute(uint32_t index) {
    bool m = false;
    for (auto& s : st_.streams)
        if (s.index == index) m = s.muted = !s.muted;
    with_lock([&] {
        if (auto* op = pa_context_set_sink_input_mute(ctx_, index, m, nullptr, nullptr)) pa_operation_unref(op);
    });
}

void Audio::set_device_volume(bool source, uint32_t index, float v) {
    v = std::clamp(v, 0.0f, 1.5f);
    uint8_t ch = 2;
    for (auto* list : {source ? &st_.sources : &st_.sinks})
        for (auto& d : *list)
            if (d.index == index) {
                d.volume = v;
                ch = std::max<uint8_t>(1, d.channels);
                if (d.is_default) (source ? st_.source : st_.sink).volume = v;
            }
    with_lock([&] {
        pa_cvolume cv;
        pa_cvolume_set(&cv, ch, static_cast<pa_volume_t>(std::lround(v * PA_VOLUME_NORM)));
        pa_operation* op = source ? pa_context_set_source_volume_by_index(ctx_, index, &cv, nullptr, nullptr)
                                  : pa_context_set_sink_volume_by_index(ctx_, index, &cv, nullptr, nullptr);
        if (op) pa_operation_unref(op);
    });
}

void Audio::toggle_device_mute(bool source, uint32_t index) {
    bool m = false;
    for (auto& d : source ? st_.sources : st_.sinks)
        if (d.index == index) {
            m = d.muted = !d.muted;
            if (d.is_default) (source ? st_.source : st_.sink).muted = m;
        }
    with_lock([&] {
        pa_operation* op = source ? pa_context_set_source_mute_by_index(ctx_, index, m, nullptr, nullptr)
                                  : pa_context_set_sink_mute_by_index(ctx_, index, m, nullptr, nullptr);
        if (op) pa_operation_unref(op);
    });
}

void Audio::move_stream(uint32_t stream, uint32_t sink) {
    for (auto& s : st_.streams)
        if (s.index == stream) s.sink = sink;
    with_lock([&] {
        if (auto* op = pa_context_move_sink_input_by_index(ctx_, stream, sink, nullptr, nullptr)) pa_operation_unref(op);
    });
}

}  // namespace island
