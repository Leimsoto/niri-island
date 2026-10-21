// Smaller panels: sound, weather, system stats, session and the timer.

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

// ── sound ──────────────────────────────────────────────────────────────────

static const char* sink_glyph(const AudioDevice& d) {
    std::string n = lower(d.name + " " + d.description + " " + d.icon);
    if (n.find("bluez") != std::string::npos || n.find("headset") != std::string::npos ||
        n.find("headphone") != std::string::npos)
        return "󰋋";
    if (n.find("hdmi") != std::string::npos || n.find("displayport") != std::string::npos) return "󰡁";
    if (n.find("usb") != std::string::npos) return "󰓃";
    return "󰓃";
}

static const char* source_glyph(const AudioDevice& d) {
    std::string n = lower(d.name + " " + d.description);
    if (n.find("bluez") != std::string::npos || n.find("headset") != std::string::npos) return "󰋎";
    if (n.find("webcam") != std::string::npos || n.find("camera") != std::string::npos) return "󰄀";
    return "󰍬";
}

// Like pavucontrol: outputs, inputs and the apps playing, each with its own
// volume; a device becomes the default with a click, an app moves to another
// output with a chip.
void App::draw_sound(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& st = audio_.state();
    w::header(c, ID("snd.back"), w, "Sonido", snd_tab_ == 1 ? st.source.description : st.sink.description)
        .click([this] { back(); });

    // Tabs, with a sliding highlight.
    const char* tabs[3] = {"Salida", "Entrada", "Aplicaciones"};
    const char* tglyph[3] = {"󰓃", "󰍬", "󰀻"};
    size_t counts[3] = {st.sinks.size(), st.sources.size(), st.streams.size()};
    Rect tb{16, 60, w - 32, 34};
    fill(cr, tb, 17, p.text.with_alpha(0.05f));
    float tw = tb.w / 3;
    snd_tab_x_.set(static_cast<float>(snd_tab_));
    fill(cr, {tb.x + 3 + snd_tab_x_.value * tw, tb.y + 3, tw - 6, tb.h - 6}, 14, p.accent_dim);
    for (int i = 0; i < 3; ++i) {
        Rect r{tb.x + i * tw, tb.y, tw, tb.h};
        uint64_t id = ID("snd.tab", i);
        float hv = ui_.hover(id);
        bool on = snd_tab_ == i;
        Color col = on ? p.accent : mix(p.text_dim, p.text, hv);
        std::string label = std::string(tabs[i]) + (counts[i] ? "  " + std::to_string(counts[i]) : "");
        Size ls = measure(label, {ui(12.5f, "SemiBold"), col});
        float x = r.cx() - (ls.w + 20) / 2;
        glyph(cr, x + 7, r.cy(), tglyph[i], 13, col);
        text(cr, x + 20, r.cy() - 8, label, {ui(12.5f, "SemiBold"), col});
        ui_.add(id, r).click([this, i] {
            snd_tab_ = i;
            snd_scroll_.reset(0);
        });
    }

    Rect area{16, 106, w - 32, h - 106 - 8};
    float y = area.y - snd_scroll_.value;
    float content = 0;
    cairo_save(cr);
    cairo_rectangle(cr, area.x - 4, area.y, area.w + 8, area.h);
    cairo_clip(cr);
    ui_.push_clip(area);

    auto empty = [&](const char* g, const char* msg) {
        glyph(cr, w / 2, area.y + 30, g, 22, p.text_faint);
        text(cr, w / 2, area.y + 50, msg, {ui(12.5f), p.text_dim, Align::Center});
        content = 92;
    };

    if (snd_tab_ < 2) {
        bool src = snd_tab_ == 1;
        const auto& list = src ? st.sources : st.sinks;
        if (list.empty()) empty(src ? "󰍭" : "󰝟", src ? "No hay micrófonos" : "No hay salidas de audio");
        int i = 0;
        for (const auto& d : list) {
            Rect r{area.x, y, area.w, 84};
            uint64_t id = ID(src ? "snd.src" : "snd.sink", i);
            float hv = ui_.hover(id);
            fill(cr, r, 18, d.is_default ? mix(p.surface, p.accent_dim, 0.55f) : mix(p.surface, p.surface_hi, 0.4f + 0.6f * hv));
            if (d.is_default) stroke(cr, r.inset(0.75f), 17.5f, p.accent.with_alpha(0.45f), 1.2f);
            Rect ic{r.x + 12, r.y + 10, 30, 30};
            fill(cr, ic, 15, d.is_default ? p.accent : p.text.with_alpha(0.08f));
            glyph(cr, ic.cx(), ic.cy(), src ? source_glyph(d) : sink_glyph(d), 14, d.is_default ? p.on_accent : p.text_dim);
            text(cr, r.x + 52, r.y + 9, d.description, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 100});
            text(cr, r.x + 52, r.y + 27, d.is_default ? "Predeterminado" : "Clic para usar este dispositivo",
                 {ui(10.5f), d.is_default ? p.accent : p.text_faint, Align::Left, r.w - 100});
            // Radio
            Rect rd{r.right() - 32, r.y + 16, 18, 18};
            stroke(cr, rd, 9, d.is_default ? p.accent : p.text_faint, 1.5f);
            if (d.is_default) fill(cr, rd.inset(4.5f), 4.5f, p.accent);
            std::string name = d.name;
            ui_.add(id, {r.x, r.y, r.w, 44}).click([this, src, name] {
                if (src) audio_.set_default_source(name);
                else audio_.set_default_sink(name);
            });
            uint32_t idx = d.index;
            w::slider(c, ID(src ? "snd.srcv" : "snd.sinkv", i), {r.x + 10, r.y + 46, r.w - 20, 32}, std::min(1.0f, d.volume),
                      d.muted ? (src ? "󰍭" : "󰝟") : (src ? "󰍬" : "󰕾"),
                      [this, src, idx](float v) { audio_.set_device_volume(src, idx, v); },
                      [this, src, idx] { audio_.toggle_device_mute(src, idx); }, "", d.muted);
            y += 92;
            content += 92;
            ++i;
        }
    } else {
        if (st.streams.empty()) empty("󰝛", "Ninguna aplicación está sonando");
        bool chips = st.sinks.size() > 1;
        int j = 0;
        for (const auto& s : st.streams) {
            float ch = chips ? 114 : 84;
            Rect r{area.x, y, area.w, ch};
            fill(cr, r, 18, mix(p.surface, p.surface_hi, 0.4f));
            Rect ic{r.x + 12, r.y + 10, 30, 30};
            cairo_surface_t* img = s.icon.empty() ? nullptr : ImageCache::get().request("icon:" + s.icon, 48);
            fill(cr, ic, 10, p.text.with_alpha(0.08f));
            if (img) image(cr, img, ic.inset(3), 6);
            else glyph(cr, ic.cx(), ic.cy(), "󰝚", 14, p.text_dim);
            text(cr, r.x + 52, r.y + 9, s.app, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 70});
            text(cr, r.x + 52, r.y + 27, s.media.empty() ? "Reproduciendo" : s.media,
                 {ui(10.5f), p.text_faint, Align::Left, r.w - 70});
            uint32_t idx = s.index;
            w::slider(c, ID("snd.app", j), {r.x + 10, r.y + 46, r.w - 20, 32}, std::min(1.0f, s.volume),
                      s.muted ? "󰝟" : "󰕾", [this, idx](float v) { audio_.set_stream_volume(idx, v); },
                      [this, idx] { audio_.toggle_stream_mute(idx); }, "", s.muted);
            if (chips) {
                // Where it plays: one chip per output.
                float cx = r.x + 12;
                int k = 0;
                for (const auto& d : st.sinks) {
                    std::string label = d.description;
                    if (utf8_length(label) > 26) {
                        size_t cut = 0;
                        for (int n = 0; n < 24; ++n) cut = utf8_next(label, cut);
                        label = label.substr(0, cut) + "…";
                    }
                    float cw = std::min(measure(label, {ui(11)}).w + 34, r.right() - 12 - cx);
                    if (cw < 60) break;
                    bool on = s.sink == d.index;
                    uint32_t sink = d.index;
                    w::pill(c, ID("snd.mv", j * 16 + k), {cx, r.y + 82, cw, 24}, label, sink_glyph(d),
                            on ? w::Style::Accent : w::Style::Plain, 11)
                        .click([this, idx, sink] { audio_.move_stream(idx, sink); });
                    cx += cw + 6;
                    ++k;
                }
            }
            y += ch + 8;
            content += ch + 8;
            ++j;
        }
    }
    ui_.pop_clip();
    cairo_restore(cr);
    ui_.add(ID("snd.area"), area).scroll([this, content, area](float dy) {
        snd_scroll_.set(std::clamp(snd_scroll_.target + dy * 60, 0.0f, std::max(0.0f, content - area.h)));
    });
    w::scrollbar(c, {w - 9, area.y + 4, 3, area.h - 8}, snd_scroll_.value, content, area.h);
}

// ── weather ────────────────────────────────────────────────────────────────

void App::draw_weather(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& d = weather_.data();
    std::string sub = d.location.empty() ? "" : d.location;
    if (d.fetched_at > 0) sub += (sub.empty() ? "" : " · ") + std::string("hace ") + time_ago(d.fetched_at);
    w::header(c, ID("wx.back"), w, "Clima", sub).click([this] { back(); });
    w::icon_button(c, ID("wx.reload"), {w - 48, 14, 32, 32}, "󰑓", 14).click([this] { weather_.reload(); });
    if (!d.valid) {
        text(cr, w / 2, h / 2 - 8, "Sin datos del clima", {ui(13), p.text_dim, Align::Center});
        return;
    }
    glyph(cr, 56, 104, d.icon, 46, p.accent);
    text(cr, 96, 72, std::to_string(d.temp) + "°", {ui(42, "Bold"), p.text});
    text(cr, 200, 76, d.condition, {ui(15, "SemiBold"), p.text, Align::Left, w - 216});
    text(cr, 200, 98, "Sensación " + std::to_string(d.feels) + "°", {ui(12), p.text_dim});
    std::string extra;
    if (d.humidity >= 0) extra += "󰖎 " + std::to_string(d.humidity) + "%   ";
    char wind[32];
    snprintf(wind, sizeof(wind), "󰖝 %.0f km/h   ", d.wind);
    extra += wind;
    if (d.uv >= 0) {
        char uv[24];
        snprintf(uv, sizeof(uv), "󰖨 UV %.0f", d.uv);
        extra += uv;
    }
    text(cr, 200, 118, extra, {mono(11.5f), p.text_dim});
    if (!d.sunrise.empty())
        text(cr, w - 18, 76, "󰖜 " + d.sunrise + "   󰖛 " + d.sunset, {mono(11), p.text_faint, Align::Right});

    // Hourly strip
    Rect hr{16, 150, w - 32, 90};
    w::card(c, hr, 16);
    size_t n = std::min<size_t>(8, d.hours.size());
    float cw = hr.w / std::max<size_t>(1, n);
    for (size_t i = 0; i < n; ++i) {
        float cx = hr.x + cw * i + cw / 2;
        const auto& hh = d.hours[i];
        text(cr, cx, hr.y + 8, hh.time, {mono(10.5f), p.text_faint, Align::Center});
        glyph(cr, cx, hr.y + 38, hh.icon, 16, p.text);
        text(cr, cx, hr.y + 50, std::to_string(hh.temp) + "°", {ui(12, "SemiBold"), p.text, Align::Center});
        if (hh.precip >= 10)
            text(cr, cx, hr.y + 68, "󰖌 " + std::to_string(hh.precip) + "%", {ui(10), p.info, Align::Center});
    }

    // Days
    Rect dr{16, hr.bottom() + 10, w - 32, h - hr.bottom() - 26};
    w::card(c, dr, 16);
    size_t nd = std::min<size_t>(6, d.days.size());
    float dw = dr.w / std::max<size_t>(1, nd);
    int lo = 100, hi = -100;
    for (size_t i = 0; i < nd; ++i) {
        lo = std::min(lo, d.days[i].min);
        hi = std::max(hi, d.days[i].max);
    }
    for (size_t i = 0; i < nd; ++i) {
        const auto& dd = d.days[i];
        float cx = dr.x + dw * i + dw / 2;
        text(cr, cx, dr.y + 8, dd.label, {ui(11.5f, "SemiBold"), i == 0 ? p.accent : p.text_dim, Align::Center});
        glyph(cr, cx, dr.y + 36, dd.icon, 15, p.text);
        text(cr, cx, dr.y + 48, std::to_string(dd.max) + "° " + std::to_string(dd.min) + "°",
             {ui(11), p.text, Align::Center});
        // Range bar relative to the week
        float span = std::max(1, hi - lo);
        float x0 = (dd.min - lo) / span, x1 = (dd.max - lo) / span;
        Rect track{cx - 18, dr.bottom() - 10, 36, 3};
        fill(cr, track, 1.5f, p.text.with_alpha(0.1f));
        fill(cr, {track.x + track.w * x0, track.y, std::max(3.0f, track.w * (x1 - x0)), 3}, 1.5f, p.warn);
    }
}

// ── stats ──────────────────────────────────────────────────────────────────

void App::draw_stats(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& s = sysmon_.stats();
    int64_t up = s.uptime_s;
    std::string upt = "Encendido " + (up >= 86400 ? std::to_string(up / 86400) + " d " : "") +
                      std::to_string((up % 86400) / 3600) + " h " + std::to_string((up % 3600) / 60) + " min";
    w::header(c, ID("st.back"), w, "Sistema", upt).click([this] { back(); });

    auto big = [&](const Rect& r, const char* g, const std::string& title, float value, const std::string& detail,
                   const std::vector<float>& hist, Color col) {
        w::card(c, r, 18);
        glyph(cr, r.x + 22, r.y + 24, g, 15, col);
        text(cr, r.x + 38, r.y + 14, title, {ui(12, "SemiBold"), p.text_dim});
        char v[16];
        snprintf(v, sizeof(v), "%.0f%%", value);
        text(cr, r.right() - 16, r.y + 10, v, {ui(20, "Bold"), p.text, Align::Right});
        text(cr, r.x + 16, r.y + 40, detail, {ui(11), p.text_faint, Align::Left, r.w - 32});
        sparkline(cr, {r.x + 12, r.y + 60, r.w - 24, r.h - 72}, hist, col);
    };
    float cw = (w - 32 - 10) / 2;
    char ram[64];
    snprintf(ram, sizeof(ram), "%.1f / %.1f GB · swap %.0f%%", s.ram_used_gb, s.ram_total_gb, s.swap);
    char cpu[64];
    snprintf(cpu, sizeof(cpu), "carga %.2f%s", s.load1,
             s.temp > 0 ? ("  ·  " + std::to_string(static_cast<int>(s.temp)) + " °C").c_str() : "");
    big({16, 62, cw, 150}, "󰍛", "Procesador", s.cpu, cpu, s.cpu_hist, s.temp > 85 ? p.bad : p.accent);
    big({16 + cw + 10, 62, cw, 150}, "󰘚", "Memoria", s.ram, ram, s.ram_hist, p.info);

    Rect net{16, 222, cw, h - 238};
    w::card(c, net, 18);
    glyph(cr, net.x + 22, net.y + 24, "󰛳", 15, p.good);
    text(cr, net.x + 38, net.y + 14, "Red", {ui(12, "SemiBold"), p.text_dim});
    text(cr, net.x + 16, net.y + 42, "↓ " + fmt_bytes_rate(s.net_down), {mono(12.5f, "SemiBold"), p.text});
    text(cr, net.x + 16, net.y + 62, "↑ " + fmt_bytes_rate(s.net_up), {mono(12.5f), p.text_dim});
    sparkline(cr, {net.x + 120, net.y + 30, net.w - 136, net.h - 42}, s.net_hist, p.good);

    Rect disk{16 + cw + 10, 222, cw, h - 238};
    w::card(c, disk, 18);
    ring(cr, disk.x + 46, disk.cy(), 26, 6, s.disk / 100.0f, s.disk > 90 ? p.bad : p.warn, p.text.with_alpha(0.1f));
    char dp[16];
    snprintf(dp, sizeof(dp), "%.0f%%", s.disk);
    text(cr, disk.x + 46, disk.cy() - 8, dp, {ui(12, "Bold"), p.text, Align::Center});
    text(cr, disk.x + 86, disk.cy() - 20, "Disco /", {ui(12, "SemiBold"), p.text_dim});
    char df[32];
    snprintf(df, sizeof(df), "%.0f GB libres", s.disk_free_gb);
    text(cr, disk.x + 86, disk.cy(), df, {ui(13, "SemiBold"), p.text});
    const auto& b = power_.battery();
    if (b.present && b.health > 0) {
        char hb[48];
        snprintf(hb, sizeof(hb), "Batería: salud %.0f%%", b.health * 100);
        text(cr, disk.x + 86, disk.cy() + 18, hb, {ui(11), p.text_faint});
    }
}

// ── session ────────────────────────────────────────────────────────────────

std::string App::session_label(const std::string& a) const {
    if (a == "lock") return "Bloquear";
    if (a == "suspend") return "Suspender";
    if (a == "logout") return "Salir";
    if (a == "reboot") return "Reiniciar";
    return "Apagar";
}

void App::run_session(const std::string& a) {
    close_all();
    const auto& c = config();
    if (a == "lock") lock_screen();
    else if (a == "suspend") proc::spawn_shell(c.cmd_suspend);
    else if (a == "logout") proc::spawn_shell(c.cmd_logout);
    else if (a == "reboot") proc::spawn_shell(c.cmd_reboot);
    else if (a == "poweroff") proc::spawn_shell(c.cmd_poweroff);
}

static const struct SessionAct {
    const char* id;
    const char* glyph;
    bool confirm;
} kSessionActs[] = {{"lock", "󰌾", false},
                    {"suspend", "󰤄", false},
                    {"logout", "󰍃", true},
                    {"reboot", "󰜉", true},
                    {"poweroff", "󰐥", true}};

void App::session_activate(int i) {
    if (i < 0 || i > 4) return;
    std::string a = kSessionActs[i].id;
    session_sel_ = i;
    if (!kSessionActs[i].confirm || confirm_ == a) {
        run_session(a);
        return;
    }
    confirm_ = a;
    loop_.cancel(confirm_timer_);
    confirm_timer_ = loop_.after(4.0, [this] {
        confirm_timer_ = 0;
        confirm_.clear();
        request();
    });
}

void App::draw_session(cairo_t* cr, float w, float h) {
    const auto& p = T();
    (void)h;
    text(cr, w / 2, 14,
         confirm_.empty() ? "¿Qué quieres hacer?  ·  ← → e Intro" : "Pulsa otra vez para " + lower(session_label(confirm_)),
         {ui(13, "SemiBold"), confirm_.empty() ? p.text_dim : p.warn, Align::Center});
    float bw = (w - 32 - 4 * 10) / 5;
    for (int i = 0; i < 5; ++i) {
        Rect r{16 + i * (bw + 10), 44, bw, 104};
        std::string a = kSessionActs[i].id;
        bool armed = confirm_ == a;
        bool chosen = session_sel_ == i;
        uint64_t id = ID("sess", i);
        float hv = std::max(ui_.hover(id), chosen ? 0.7f : 0.0f);
        float s = w::press_scale(ui_, id, 0.05f);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s);
        Color bg = armed ? p.bad.with_alpha(0.28f) : mix(p.surface, p.surface_hi, hv);
        fill(cr, r, 20, bg);
        if (armed) stroke(cr, r, 20, p.bad.with_alpha(0.7f), 1.5f);
        else if (chosen) stroke(cr, r, 20, p.accent.with_alpha(0.8f), 1.5f);
        glyph(cr, r.cx(), r.y + 42, kSessionActs[i].glyph, 24, armed ? p.bad : mix(p.text_dim, p.text, hv));
        text(cr, r.cx(), r.y + 70, session_label(a), {ui(12.5f, "SemiBold"), p.text, Align::Center});
        cairo_restore(cr);
        ui_.add(id, r).click([this, i] { session_activate(i); });
    }
}

// ── timer ──────────────────────────────────────────────────────────────────

static std::string fmt_stopwatch(double t, bool centis) {
    int64_t cs = static_cast<int64_t>(t * 100);
    int64_t sec = cs / 100;
    char buf[40];
    if (sec >= 3600)
        snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", (long long)(sec / 3600), (long long)(sec % 3600 / 60),
                 (long long)(sec % 60));
    else if (centis)
        snprintf(buf, sizeof(buf), "%02lld:%02lld.%02lld", (long long)(sec / 60), (long long)(sec % 60), (long long)(cs % 100));
    else
        snprintf(buf, sizeof(buf), "%02lld:%02lld", (long long)(sec / 60), (long long)(sec % 60));
    return buf;
}

void App::timer_mode(int mode, const Rect& from) {
    tm_mode_ = mode;
    tm_from_ = from;
    tm_anim_.reset(Spring::speed > 0 ? 0.0f : 1.0f);
    tm_anim_.set(1.0f);
    request();
}

// The panel opens on a choice; the chosen card grows into its panel.
void App::draw_timer(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    float a = std::clamp(tm_anim_.value, 0.0f, 1.0f);

    if (tm_mode_ == 0) {
        cairo_save(cr);
        if (a < 0.999f) cairo_push_group(cr);
        w::header(c, ID("tm.back"), w, "Reloj", "Cuenta atrás o cronómetro").click([this] { back(); });
        float cw = (w - 16 * 2 - 12) / 2;
        for (int i = 0; i < 2; ++i) {
            Rect r{16 + i * (cw + 12), 64, cw, h - 64 - 16};
            tm_cards_[i] = r;
            uint64_t id = ID("tm.card", i);
            float hv = ui_.hover(id);
            float sc = w::press_scale(ui_, id, 0.04f) * (0.94f + 0.06f * a);
            bool active = i == 0 ? timer_.running() : stopwatch_.running();
            cairo_save(cr);
            scale_around(cr, r.cx(), r.cy(), sc);
            fill(cr, r, 20, mix(p.surface, p.surface_hi, 0.4f + 0.6f * hv));
            if (active) stroke(cr, r.inset(0.75f), 19.5f, p.accent.with_alpha(0.55f), 1.5f);
            Rect ic{r.x + 16, r.y + 16, 40, 40};
            fill(cr, ic, 20, active ? p.accent : p.accent_dim);
            glyph(cr, ic.cx(), ic.cy(), i == 0 ? "󱎫" : "󱫠", 19, active ? p.on_accent : p.accent);
            text(cr, r.x + 16, r.y + 66, i == 0 ? "Temporizador" : "Cronómetro", {ui(15, "Bold"), p.text});
            std::string sub;
            if (i == 0) sub = timer_.running() ? (timer_.paused() ? "En pausa · " : "Quedan ") + fmt_duration((int64_t)std::ceil(timer_.remaining()))
                                               : "Cuenta atrás a medida";
            else sub = stopwatch_.running() ? (stopwatch_.paused() ? "En pausa · " : "En marcha · ") + fmt_stopwatch(stopwatch_.elapsed(), false)
                                            : "Mide tiempo y vueltas";
            text(cr, r.x + 16, r.y + 88, sub, {ui(11.5f), active ? p.accent : p.text_dim, Align::Left, r.w - 32});
            glyph(cr, r.right() - 22, r.y + 26, "󰁔", 14, p.text_faint.fade(0.4f + 0.6f * hv));
            cairo_restore(cr);
            ui_.add(id, r).click([this, i, r] { timer_mode(i + 1, r); });
        }
        if (a < 0.999f) {
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, smoothstep(a));
        }
        cairo_restore(cr);
        return;
    }

    // Growing out of the card that was clicked.
    bool grouped = false;
    if (a < 0.999f && !tm_from_.empty()) {
        Rect full{8, 8, w - 16, h - 16};
        float k = smoothstep(a);
        Rect r{lerp(tm_from_.x, full.x, k), lerp(tm_from_.y, full.y, k), lerp(tm_from_.w, full.w, k),
               lerp(tm_from_.h, full.h, k)};
        fill(cr, r, lerp(20, 22, k), p.surface.fade(1.0f - k * 0.6f));
        cairo_save(cr);
        cairo_push_group(cr);
        grouped = true;
    }
    if (tm_mode_ == 1) {
        if (timer_.running()) {
            float frac = timer_.progress();
            double rem = timer_.remaining();
            Color col = rem <= 30 ? p.bad : rem <= 120 ? p.warn : p.accent;
            if (timer_.paused()) col = p.text_dim;
            std::string label = timer_.label().empty() ? "Temporizador" : timer_.label();
            w::header(c, ID("tm.back"), w, label, timer_.paused() ? "En pausa"
                                                  : "Termina a las " + fmt_time("%H:%M", time(nullptr) + (time_t)rem))
                .click([this] { timer_mode(0, {}); });
            w::icon_button(c, ID("tm.restart"), {w - 52, 14, 34, 34}, "󰑓", 14).click([this] { timer_.restart(); });
            float cx = 84, cy = 134;
            ring(cr, cx, cy, 52, 8, frac, col, p.text.with_alpha(0.08f));
            std::string t = fmt_duration(static_cast<int64_t>(std::ceil(rem)));
            text(cr, cx, cy - 12, t, {mono(t.size() > 5 ? 17 : 22, "Bold"), p.text, Align::Center});
            float bx = 160, bw = w - bx - 18;
            w::pill(c, ID("tm.toggle"), {bx, 96, bw, 38}, timer_.paused() ? "Seguir" : "Pausa",
                    timer_.paused() ? "󰐊" : "󰏤", w::Style::Accent)
                .click([this] { timer_.toggle(); });
            float sw = (bw - 16) / 3;
            w::pill(c, ID("tm.add1"), {bx, 142, sw, 34}, "+1 min", nullptr, w::Style::Tonal).click([this] { timer_.add(60); });
            w::pill(c, ID("tm.add5"), {bx + sw + 8, 142, sw, 34}, "+5 min", nullptr, w::Style::Tonal).click([this] { timer_.add(300); });
            w::pill(c, ID("tm.cancel"), {bx + 2 * (sw + 8), 142, sw, 34}, "Parar", "󰓛", w::Style::Danger)
                .click([this] { timer_.cancel(); });
        } else {
            draw_timer_setup(cr, w, h);
        }
    } else {
        draw_stopwatch(cr, w, h);
    }
    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, smoothstep((a - 0.35f) / 0.65f));
        cairo_restore(cr);
    }
}

void App::draw_timer_setup(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    (void)h;
    w::header(c, ID("tm.back"), w, "Temporizador", "Ajusta la duración").click([this] { timer_mode(0, {}); });

    // Three wheels: hours, minutes, seconds. Arrows, scroll, or a click on the number.
    int* vals[3] = {&tm_h_, &tm_m_, &tm_s_};
    int maxv[3] = {23, 59, 59};
    const char* units[3] = {"horas", "min", "seg"};
    float colw = 92, gap = 26;
    float x0 = (w - (3 * colw + 2 * gap)) / 2;
    for (int i = 0; i < 3; ++i) {
        float x = x0 + i * (colw + gap);
        int* v = vals[i];
        int mx = maxv[i];
        auto bump = [this, v, mx](int d) {
            *v = (*v + d + mx + 1) % (mx + 1);
            request();
        };
        w::icon_button(c, ID("tm.up", i), {x + colw / 2 - 16, 60, 32, 26}, "󰅃", 16).click([bump] { bump(1); });
        Rect box{x, 90, colw, 66};
        uint64_t bid = ID("tm.box", i);
        float hv = ui_.hover(bid);
        fill(cr, box, 18, mix(p.surface, p.surface_hi, 0.5f + 0.5f * hv));
        char num[8];
        snprintf(num, sizeof(num), "%02d", *v);
        text(cr, box.cx(), box.y + 8, num, {mono(36, "Bold"), *v ? p.text : p.text_faint, Align::Center});
        text(cr, box.cx(), box.bottom() + 30, units[i], {ui(10.5f, "SemiBold"), p.text_faint, Align::Center});
        ui_.add(bid, box)
            .scroll([bump](float dy) { bump(dy > 0 ? -1 : 1); })
            .click([bump] { bump(1); })
            .right([bump] { bump(-1); });
        w::icon_button(c, ID("tm.dn", i), {x + colw / 2 - 16, 160, 32, 26}, "󰅀", 16).click([bump] { bump(-1); });
        if (i < 2) text(cr, x + colw + gap / 2, 100, ":", {mono(30, "Bold"), p.text_faint, Align::Center});
    }

    // Quick picks fill the wheels.
    struct Q {
        const char* label;
        int h, m;
    } qs[] = {{"1 min", 0, 1}, {"5 min", 0, 5}, {"10 min", 0, 10}, {"15 min", 0, 15}, {"25 min", 0, 25}, {"1 h", 1, 0}};
    float qw = (w - 32 - 5 * 6) / 6;
    for (int i = 0; i < 6; ++i) {
        bool on = tm_h_ == qs[i].h && tm_m_ == qs[i].m && tm_s_ == 0;
        int hh = qs[i].h, mm = qs[i].m;
        w::pill(c, ID("tm.q", i), {16 + i * (qw + 6), 212, qw, 30}, qs[i].label, nullptr,
                on ? w::Style::Accent : w::Style::Plain, 11.5f)
            .click([this, hh, mm] {
                tm_h_ = hh;
                tm_m_ = mm;
                tm_s_ = 0;
                request();
            });
    }

    int total = tm_h_ * 3600 + tm_m_ * 60 + tm_s_;
    std::string go = total ? "Iniciar  ·  " + fmt_duration(total) : "Elige una duración";
    w::pill(c, ID("tm.start"), {16, 254, w - 32, 44}, go, total ? "󰐊" : nullptr,
            total ? w::Style::Accent : w::Style::Plain, 13.5f)
        .click([this, total] {
            if (!total) return;
            timer_.start(total, tm_m_ == 25 && !tm_h_ && !tm_s_ ? "Pomodoro" : "");
        });
}

void App::draw_stopwatch(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    bool run = stopwatch_.running(), paused = stopwatch_.paused();
    w::header(c, ID("tm.back"), w, "Cronómetro",
              !run ? "Listo para empezar" : paused ? "En pausa" : std::to_string(stopwatch_.laps().size() + 1) + "ª vuelta")
        .click([this] { timer_mode(0, {}); });

    // The figure: minutes:seconds large, hundredths beside it.
    double t = stopwatch_.elapsed();
    std::string full = fmt_stopwatch(t, true);
    size_t dot = full.find('.');
    std::string big = dot == std::string::npos ? full : full.substr(0, dot);
    std::string small = dot == std::string::npos ? "" : full.substr(dot);
    Text bt{mono(44, "Bold"), paused ? p.text_dim : p.text};
    Size bs = measure(big, bt);
    Size ss = measure(small, {mono(22, "Bold"), p.text_faint});
    float x = (w - bs.w - ss.w) / 2;
    text(cr, x, 60, big, bt);
    text(cr, x + bs.w, 78, small, {mono(22, "Bold"), run && !paused ? p.accent : p.text_faint});

    float by = 136, bw = 132;
    Rect left{w / 2 - bw - 6, by, bw, 42}, right{w / 2 + 6, by, bw, 42};
    if (!run) {
        w::pill(c, ID("sw.go"), {w / 2 - bw / 2, by, bw, 42}, "Iniciar", "󰐊", w::Style::Accent, 13.5f)
            .click([this] { stopwatch_.start(); });
    } else {
        if (paused)
            w::pill(c, ID("sw.l"), left, "Reiniciar", "󰑓", w::Style::Tonal, 13).click([this] { stopwatch_.reset(); });
        else
            w::pill(c, ID("sw.l"), left, "Vuelta", "󰈻", w::Style::Tonal, 13).click([this] { stopwatch_.lap(); });
        w::pill(c, ID("sw.r"), right, paused ? "Seguir" : "Pausa", paused ? "󰐊" : "󰏤",
                paused ? w::Style::Accent : w::Style::Plain, 13)
            .click([this] { stopwatch_.toggle(); });
    }

    // Laps, newest first; fastest in green and slowest in red once there are three.
    const auto& laps = stopwatch_.laps();
    if (laps.empty()) return;
    std::vector<double> d(laps.size());
    for (size_t i = 0; i < laps.size(); ++i) d[i] = laps[i] - (i ? laps[i - 1] : 0);
    size_t best = 0, worst = 0;
    for (size_t i = 0; i < d.size(); ++i) {
        if (d[i] < d[best]) best = i;
        if (d[i] > d[worst]) worst = i;
    }
    float y = 196;
    divider(cr, 20, y - 6, w - 20, y - 6, p.border);
    for (size_t k = 0; k < laps.size() && y + 26 < h; ++k, y += 30) {
        size_t i = laps.size() - 1 - k;
        Color col = laps.size() >= 3 && i == best ? p.good : laps.size() >= 3 && i == worst ? p.bad : p.text;
        text(cr, 24, y + 4, "Vuelta " + std::to_string(i + 1), {ui(12.5f), p.text_dim});
        text(cr, w / 2 + 40, y + 4, fmt_stopwatch(d[i], true), {mono(13, "Bold"), col, Align::Right});
        text(cr, w - 24, y + 4, fmt_stopwatch(laps[i], true), {mono(12), p.text_faint, Align::Right});
    }
}

// The running clock, as a strip at the bottom of the hover summaries: the
// timer when there is one, otherwise the stopwatch.
void App::draw_summary_strips(cairo_t* cr, float w, float y, float step) {
    if (recorder_.active()) {  // first: it is the one that must be easy to stop
        draw_rec_strip(cr, {12, y, w - 24, 46});
        y += step;
    }
    if (clock_active()) draw_clock_strip(cr, {12, y, w - 24, 46});
}

void App::draw_rec_strip(cairo_t* cr, const Rect& r) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    auto st = recorder_.state();
    bool paused = st == Recorder::State::Paused;
    fill(cr, r, r.h / 2, p.bad.with_alpha(0.12f));
    float cx = r.x + r.h / 2, cy = r.cy();
    float pulse = st == Recorder::State::Recording ? 0.55f + 0.45f * std::fabs(std::sin(anim_time_ * 2.2f)) : 1.0f;
    Color tint = paused ? p.warn : p.bad;
    if (paused) glyph(cr, cx, cy, "󰏤", 14, tint);
    else fill(cr, {cx - 7, cy - 7, 14, 14}, 7, tint.fade(pulse));
    std::string t = st == Recorder::State::Countdown ? std::to_string(recorder_.countdown())
                                                     : fmt_duration(static_cast<int64_t>(recorder_.elapsed()));
    text(cr, r.x + r.h + 4, r.y + 5, t, {mono(16, "Bold"), tint});
    const char* state = st == Recorder::State::Countdown ? "Grabación · empieza ya"
                      : st == Recorder::State::Saving    ? "Grabación · guardando…"
                      : paused                           ? "Grabación · en pausa"
                                                         : "Grabando la pantalla";
    text(cr, r.x + r.h + 4, r.y + 25, state, {ui(10.5f), p.text_dim, Align::Left, r.w - r.h - 130});
    if (st == Recorder::State::Saving) return;
    // pause/resume, microphone, stop (and save)
    float bx = r.right() - 3 * 34 - 6;
    w::icon_button(c, ID("rstrip.pp"), {bx, r.cy() - 16, 32, 32}, paused ? "󰐊" : "󰏤", 14, !paused)
        .click([this] { recorder_.toggle_pause(); });
    bool mic_muted = audio_.state().source.muted;
    w::icon_button(c, ID("rstrip.mic"), {bx + 34, r.cy() - 16, 32, 32}, mic_muted ? "󰍭" : "󰍬", 14)
        .click([this] { audio_.toggle_mic_mute(); });
    w::icon_button(c, ID("rstrip.stop"), {bx + 68, r.cy() - 16, 32, 32}, "󰓛", 14, false, p.bad).click([this] {
        recorder_.stop();
    });
    // the rest of the strip: all the recording's controls
    ui_.add(ID("rstrip.open"), {r.x, r.y, bx - r.x, r.h}).click([this] { open(View::Record); });
}

void App::draw_clock_strip(cairo_t* cr, const Rect& r) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    bool tm = timer_.running();
    bool paused = tm ? timer_.paused() : stopwatch_.paused();
    fill(cr, r, r.h / 2, p.text.with_alpha(0.06f));
    float cx = r.x + r.h / 2, cy = r.cy();
    if (tm) {
        double rem = timer_.remaining();
        Color col = paused ? p.text_dim : rem <= 30 ? p.bad : rem <= 120 ? p.warn : p.accent;
        ring(cr, cx, cy, 13, 3.5f, timer_.progress(), col, p.text.with_alpha(0.1f));
        glyph(cr, cx, cy, paused ? "󰏤" : "󱎫", 12, col);
        text(cr, r.x + r.h + 4, r.y + 5, fmt_duration((int64_t)std::ceil(rem)), {mono(16, "Bold"), p.text});
        std::string sub = (timer_.label().empty() ? std::string("Temporizador") : timer_.label()) + " · " +
                          (paused ? std::string("en pausa") : "termina " + fmt_time("%H:%M", time(nullptr) + (time_t)rem));
        text(cr, r.x + r.h + 4, r.y + 25, sub, {ui(10.5f), p.text_dim, Align::Left, r.w - r.h - 130});
    } else {
        // A dot circling the dial: one turn per minute.
        double t = stopwatch_.elapsed();
        float ang = static_cast<float>(std::fmod(t, 60.0) / 60.0);
        ring(cr, cx, cy, 13, 3.5f, ang, paused ? p.text_dim : p.accent, p.text.with_alpha(0.1f));
        glyph(cr, cx, cy, "󱫠", 12, paused ? p.text_dim : p.accent);
        text(cr, r.x + r.h + 4, r.y + 5, fmt_stopwatch(t, false), {mono(16, "Bold"), p.text});
        std::string sub = "Cronómetro · " + (paused ? std::string("en pausa")
                                                    : std::to_string(stopwatch_.laps().size() + 1) + "ª vuelta");
        text(cr, r.x + r.h + 4, r.y + 25, sub, {ui(10.5f), p.text_dim, Align::Left, r.w - r.h - 130});
    }
    // Controls on the right: pause/resume, a shortcut (+1 min / lap), stop.
    float bx = r.right() - 3 * 34 - 6;
    w::icon_button(c, ID("strip.pp"), {bx, r.cy() - 16, 32, 32}, paused ? "󰐊" : "󰏤", 14, !paused).click([this, tm] {
        if (tm) timer_.toggle();
        else stopwatch_.toggle();
    });
    w::icon_button(c, ID("strip.mid"), {bx + 34, r.cy() - 16, 32, 32}, tm ? "󰐕" : "󰈻", 14).click([this, tm] {
        if (tm) timer_.add(60);
        else stopwatch_.lap();
    });
    w::icon_button(c, ID("strip.stop"), {bx + 68, r.cy() - 16, 32, 32}, "󰓛", 14, false, p.bad).click([this, tm] {
        if (tm) timer_.cancel();
        else stopwatch_.reset();
    });
    // The rest of the strip opens the panel.
    ui_.add(ID("strip.open"), {r.x, r.y, bx - r.x, r.h}).click([this] { open(View::Timer); });
}

}  // namespace island
