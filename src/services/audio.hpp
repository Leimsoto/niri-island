#pragma once

#include <functional>
#include <pulse/pulseaudio.h>
#include <string>
#include <vector>

namespace island {

struct AudioDevice {
    uint32_t index = 0;
    std::string name, description, icon;
    float volume = 0;   // 1.0 = 100 %
    bool muted = false;
    bool is_default = false;
    uint8_t channels = 2;
};

struct AudioStream {
    uint32_t index = 0;
    std::string app, icon, media;
    uint32_t sink = 0;  // the device it plays on
    float volume = 0;
    bool muted = false;
    uint8_t channels = 2;
};

struct AudioState {
    bool ready = false;
    AudioDevice sink, source;           // the defaults
    std::vector<AudioDevice> sinks, sources;
    std::vector<AudioStream> streams;   // playback (sink inputs)
    std::vector<std::string> mic_apps;  // apps recording right now (privacy)
};

// Talks to PulseAudio / pipewire-pulse. libpulse runs its own thread; every
// callback copies data and posts it to the loop, so the state is only ever
// touched on the main thread.
class Audio {
public:
    ~Audio();
    bool init();

    const AudioState& state() const { return st_; }
    bool mic_in_use() const { return !st_.mic_apps.empty(); }

    void set_volume(float v);           // default sink, 0..1.5
    void change_volume(float delta);
    void toggle_mute();
    void set_mic_volume(float v);
    void toggle_mic_mute();
    void set_default_sink(const std::string& name);
    void set_default_source(const std::string& name);
    void set_stream_volume(uint32_t index, float v);
    void toggle_stream_mute(uint32_t index);
    // Any device by index, like pavucontrol: volume, mute, where an app plays.
    void set_device_volume(bool source, uint32_t index, float v);
    void toggle_device_mute(bool source, uint32_t index);
    void move_stream(uint32_t stream, uint32_t sink);

    // Fired on the main thread. `user` is true for changes worth an OSD
    // (volume/mute of the default sink, after start-up and device switches).
    std::function<void(bool user)> on_sink_change;
    std::function<void()> on_change;
    std::function<void(bool muted)> on_mic_mute;

private:
    pa_threaded_mainloop* ml_ = nullptr;
    pa_context* ctx_ = nullptr;
    AudioState st_;
    std::string default_sink_, default_source_;
    double armed_at_ = 0;
    double suppress_until_ = 0;
    uint64_t reconnect_timer_ = 0;

    void connect();
    void disconnect();
    void refresh_all();  // pulse thread, under lock

    static void ctx_state(pa_context* c, void* ud);
    static void ctx_event(pa_context* c, pa_subscription_event_type_t t, uint32_t idx, void* ud);
    static void server_cb(pa_context* c, const pa_server_info* i, void* ud);
    static void sinks_cb(pa_context* c, const pa_sink_info* i, int eol, void* ud);
    static void sources_cb(pa_context* c, const pa_source_info* i, int eol, void* ud);
    static void inputs_cb(pa_context* c, const pa_sink_input_info* i, int eol, void* ud);
    static void outputs_cb(pa_context* c, const pa_source_output_info* i, int eol, void* ud);

    // Accumulators filled on the pulse thread between list callbacks.
    std::vector<AudioDevice> acc_sinks_, acc_sources_;
    std::vector<AudioStream> acc_streams_;
    std::vector<std::string> acc_mic_;
    std::vector<uint32_t> monitor_sources_;  // pulse thread only

    void with_lock(const std::function<void()>& f);
};

}  // namespace island
