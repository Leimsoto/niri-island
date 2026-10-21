// The island's own layers: rest (clock + running activities), the glance, the
// player, notifications, OSDs, and the workspaces bubble beside it.

#include "core/config.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

// No leading space for one-digit hours ("%l" pads): it would push the clock off-centre.
static std::string clock_text() { return trim(fmt_time(config().clock_24h ? "%H:%M" : "%l:%M")); }

Color App::media_accent() {
    const auto& m = mpris_.info();
    if (m.art_url.empty()) return T().accent;
    ImageCache::get().request(m.art_url, 160);  // the accent comes from the large copy
    return ImageCache::get().color_of(m.art_url, 160, T().accent);
}

void App::draw_art(cairo_t* cr, const Rect& r, float radius) {
    const auto& p = T();
    const auto& m = mpris_.info();
    // A copy decoded near the drawn size: scaling 160 px down to 22 px every frame is costly.
    int want = r.w <= 32 ? static_cast<int>(std::lround(r.w * scale())) : 160;
    cairo_surface_t* img = m.art_url.empty() ? nullptr : ImageCache::get().request(m.art_url, want);
    if (!img && want != 160 && !m.art_url.empty()) img = ImageCache::get().request(m.art_url, 160);
    if (img) {
        image(cr, img, r, radius);
        stroke(cr, r, radius, p.text.with_alpha(0.08f), 1.0f);
    } else {
        cairo_pattern_t* g = cairo_pattern_create_linear(r.x, r.y, r.right(), r.bottom());
        Color a = mix(p.accent_dim, p.accent, 0.35f), b = p.surface_hi;
        cairo_pattern_add_color_stop_rgba(g, 0, a.r, a.g, a.b, 1);
        cairo_pattern_add_color_stop_rgba(g, 1, b.r, b.g, b.b, 1);
        rrect(cr, r, radius);
        cairo_set_source(cr, g);
        cairo_fill(cr);
        cairo_pattern_destroy(g);
        glyph(cr, r.cx(), r.cy(), "󰎆", r.h * 0.42f, p.text.with_alpha(0.85f));
    }
}

void App::draw_spectrum(cairo_t* cr, const Rect& r, int bars, bool playing, Color c) {
    const auto& live = cava_.bars();
    bool real = cava_.live();
    float bw = std::max(1.6f, (r.w - (bars - 1) * 2.0f) / bars);
    float gap = bars > 1 ? (r.w - bars * bw) / (bars - 1) : 0;
    static const float freqs[] = {6.2f, 8.5f, 7.1f, 9.3f, 5.7f, 7.9f, 8.8f, 6.6f};
    for (int i = 0; i < bars; ++i) {
        float v;
        if (!playing) {
            v = 0.12f;
        } else if (real) {
            // Average the cava bands that fall into this bar.
            int per = std::max(1, Cava::kBars / bars);
            float sum = 0;
            for (int k = 0; k < per; ++k) sum += live[std::min(Cava::kBars - 1, i * per + k)];
            v = 0.12f + 0.88f * std::sqrt(sum / per);
        } else {
            float ph = i * 1.7f;
            v = 0.5f + 0.32f * std::sin(anim_time_ * freqs[i % 8] + ph) + 0.15f * std::cos(anim_time_ * 12.0f + ph * 1.8f);
            v = 0.18f + 0.82f * clamp01(v);
        }
        float bh = std::max(bw, r.h * clamp01(v));
        fill(cr, {r.x + i * (bw + gap), r.cy() - bh / 2, bw, bh}, bw / 2, c.with_alpha(playing ? 0.95f : 0.45f));
    }
}

// ── rest ───────────────────────────────────────────────────────────────────

void App::draw_rest(cairo_t* cr, float w, float h) {
    // "One island": workspaces on the left, status on the right, the usual
    // content in between.
    if (float side = one_island_side(); side > 0) {
        draw_island_sides(cr, w, h);
        cairo_save(cr);
        cairo_translate(cr, side, 0);
        ui_.push(side, 0);
        draw_rest_inner(cr, w - 2 * side, h);
        ui_.pop();
        cairo_restore(cr);
        return;
    }
    draw_rest_inner(cr, w, h);
}

void App::draw_rest_inner(cairo_t* cr, float w, float h) {
    const auto& p = T();
    const auto& m = mpris_.info();
    w::Ctx c{cr, ui_};
    bool mic = audio_.mic_in_use();
    bool tm = clock_active();
    bool sw = !timer_.running();  // the stopwatch is shown only without a timer

    // Clock in the middle, always.
    text(cr, w / 2, h / 2 - 9.5f, clock_text(), {mono(15, "Bold"), p.text, Align::Center});

    // A recording takes both sides: it is the one thing nothing else can stop.
    if (recorder_.active()) {
        auto st = recorder_.state();
        float pulse = st == Recorder::State::Recording ? 0.55f + 0.45f * std::fabs(std::sin(anim_time_ * 2.2f)) : 1.0f;
        uint64_t lid = ID("rest.rec");
        float hv = ui_.hover(lid);
        Rect lead{9, (h - 22) / 2, 22, 22};
        if (st == Recorder::State::Paused) {
            fill(cr, {lead.cx() - 5, lead.cy() - 6, 3.5f, 12}, 1, p.warn);
            fill(cr, {lead.cx() + 1.5f, lead.cy() - 6, 3.5f, 12}, 1, p.warn);
        } else {
            float d = 10 + hv;
            fill(cr, {lead.cx() - d / 2, lead.cy() - d / 2, d, d}, hv > 0.5f ? 2.5f : d / 2, p.bad.fade(pulse));
        }
        std::string t = st == Recorder::State::Countdown ? std::to_string(recorder_.countdown())
                      : st == Recorder::State::Saving    ? "…"
                                                         : fmt_duration(static_cast<int64_t>(recorder_.elapsed()));
        text(cr, w - 12, h / 2 - 8.5f, t, {mono(13, "Bold"), st == Recorder::State::Paused ? p.warn : p.bad, Align::Right});
        // The dot stops (it squares off under the pointer); the time opens the controls.
        ui_.add(lid, {0, 0, 40, h}).click([this] { recorder_.stop(); });
        ui_.add(ID("rest.rectime"), {w - 80, 0, 80, h}).click([this] { open(View::Record); });
        return;
    }

    // Leading slot: the track's art, or the microphone in use.
    Rect lead{9, (h - 22) / 2, 22, 22};
    if (m.active) {
        draw_art(cr, lead, 6.5f);
        if (mic) fill(cr, {3.5f, h / 2 - 2.5f, 5, 5}, 2.5f, p.warn);
    } else if (mic) {
        fill(cr, lead, 11, p.warn.with_alpha(0.18f));
        glyph(cr, lead.cx(), lead.cy(), "󰍬", 13, p.warn);
    } else if (tm) {
        bool paused = sw ? stopwatch_.paused() : timer_.paused();
        glyph(cr, lead.cx(), lead.cy(), paused ? "󰏤" : sw ? "󱫠" : "󱎫", 15, p.accent);
    }

    // Trailing slot: the countdown, else the spectrum, else who is recording.
    Rect trail{w - 9 - 64, 0, 64, h};
    if (tm) {
        double rem = sw ? stopwatch_.elapsed() : timer_.remaining();
        Color tint = sw ? p.accent : rem <= 30 ? p.bad : rem <= 120 ? p.warn : p.accent;
        if (sw ? stopwatch_.paused() : timer_.paused()) tint = p.text_dim;
        std::string t = fmt_duration(static_cast<int64_t>(sw ? rem : std::ceil(rem)));
        text(cr, w - 12, h / 2 - 8.5f, t, {mono(13, "Bold"), tint, Align::Right});
        w::hit(c, ID("rest.timer"), trail).click([this] { open(View::Timer); });
    } else if (m.active) {
        draw_spectrum(cr, {w - 11 - 22, h / 2 - 8, 22, 16}, 4, m.playing, media_accent());
    } else if (mic) {
        std::string who = audio_.state().mic_apps.empty() ? "" : audio_.state().mic_apps.front();
        text(cr, w - 12, h / 2 - 8, who, {ui(11.5f, "SemiBold"), p.warn, Align::Right});
    }
}

// ── glance ─────────────────────────────────────────────────────────────────

void App::draw_glance(cairo_t* cr, float w, float h) {
    const auto& p = T();
    (void)h;
    std::string hm = clock_text();
    Size cs = text(cr, 20, 14, hm, {mono(32, "Bold"), p.text});
    text(cr, 20 + cs.w + 4, 22, fmt_time("%S"), {mono(13, "SemiBold"), p.text_faint});
    std::string date = fmt_time("%A, %e de %B");
    if (!date.empty()) date[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(date[0])));
    text(cr, 21, 58, date, {ui(12), p.text_dim, Align::Left, 160});

    float dx = 196;
    divider(cr, dx - 14, 16, dx - 14, 76, p.border);
    draw_summary_strips(cr, w, 92, 54);

    const auto& wd = weather_.data();
    if (wd.valid) {
        glyph(cr, dx + 11, 26, wd.icon, 20, p.accent);
        Size ts = text(cr, dx + 28, 15, std::to_string(wd.temp) + "°", {ui(17, "Bold"), p.text});
        text(cr, dx + 32 + ts.w, 19, wd.condition, {ui(11.5f), p.text_dim, Align::Left, w - dx - 50 - ts.w});
    } else {
        text(cr, dx, 17, "Sin datos del clima", {ui(12), p.text_faint});
    }

    // Battery and processor, then the next task.
    const auto& b = power_.battery();
    std::string line;
    if (b.present) line += std::string(b.charging ? "󰂄 " : "󰁹 ") + std::to_string(b.percent) + "%   ";
    line += "󰍛 " + std::to_string(static_cast<int>(std::lround(sysmon_.stats().cpu))) + "%   ";
    line += "󰘚 " + std::to_string(static_cast<int>(std::lround(sysmon_.stats().ram))) + "%";
    text(cr, dx, 44, line, {mono(11.5f), p.text_dim, Align::Left, w - dx - 14});

    const Task* next = nullptr;
    for (auto& t : store_.tasks())
        if (!t.done() && (!next || t.date < next->date)) next = &t;
    std::string tl = next ? "󰄱  " + next->title + "  ·  " + Store::due_label(next->date) : "󰄬  Sin tareas pendientes";
    Color tc = next && next->date < Store::today() ? p.warn : p.text_faint;
    text(cr, dx, 64, tl, {ui(11.5f), tc, Align::Left, w - dx - 14});
}

// ── music ──────────────────────────────────────────────────────────────────

void App::draw_music(cairo_t* cr, float w, float h) {
    const auto& p = T();
    const auto& m = mpris_.info();
    w::Ctx c{cr, ui_};
    Color acc = media_accent();
    draw_summary_strips(cr, w, 164, 54);
    (void)h;

    // A wash of the art's colour behind everything.
    cairo_pattern_t* g = cairo_pattern_create_radial(60, 40, 10, 60, 40, w * 0.8);
    cairo_pattern_add_color_stop_rgba(g, 0, acc.r, acc.g, acc.b, 0.16);
    cairo_pattern_add_color_stop_rgba(g, 1, acc.r, acc.g, acc.b, 0.0);
    // Filled, not painted: the island's still-state clip is rectangular, and a
    // paint would light up the square corners outside its rounded shape.
    // The island's own outline: docked (square top, ears) or a capsule.
    island_outline(cr, {0, 0, w, h}, shape_radius(h), attached() && !anchored_);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);

    Rect art{16, 16, 74, 74};
    draw_art(cr, art, 15);
    w::hit(c, ID("music.art"), art).click([this] {
        if (!niri_.focus_app(mpris_.info().desktop.empty() ? mpris_.info().identity : mpris_.info().desktop))
            mpris_.raise();
    });

    float tx = 104, tw = w - tx - 60;
    text(cr, tx, 20, m.title, {ui(15.5f, "Bold"), p.text, Align::Left, tw});
    text(cr, tx, 42, m.artist.empty() ? "Artista desconocido" : m.artist, {ui(12.5f), p.text_dim, Align::Left, tw});
    // Which player, as a small chip.
    std::string chip = m.identity;
    if (!m.album.empty()) chip += "  ·  " + m.album;
    text(cr, tx, 64, "󰓃  " + chip, {ui(11), acc.with_alpha(0.9f), Align::Left, w - tx - 18});
    draw_spectrum(cr, {w - 16 - 34, 22, 34, 22}, 6, m.playing, acc);

    // Progress, draggable.
    float row = 110;
    int64_t pos = mpris_.position();
    std::string el = fmt_duration(pos / 1000000), tot = m.length_us > 0 ? fmt_duration(m.length_us / 1000000) : "--:--";
    text(cr, 16, row - 8, el, {mono(11), p.text_dim});
    text(cr, w - 16, row - 8, tot, {mono(11), p.text_dim, Align::Right});
    Rect bar{62, row - 2.5f, w - 124, 5};
    uint64_t sid = ID("music.seek");
    float sv = ui_.hover(sid);
    Rect thick = bar.inset(0, -sv * 1.5f);
    fill(cr, thick, thick.h / 2, p.text.with_alpha(0.12f));
    float frac = mpris_.progress();
    if (frac > 0) fill(cr, {thick.x, thick.y, std::max(thick.h, thick.w * frac), thick.h}, thick.h / 2, acc);
    float kx = bar.x + bar.w * frac;
    fill(cr, {kx - 6 - sv, row - 6 - sv, 12 + 2 * sv, 12 + 2 * sv}, 6 + sv, Color{1, 1, 1, 0.95f});
    if (m.can_seek) {
        ui_.add(sid, bar.inset(0, -9))
            .drag([this](const DragEvent& e) { mpris_.set_scrub(clamp01(e.fx)); })
            .release([this](const DragEvent& e) { mpris_.end_scrub(clamp01(e.fx)); });
    }

    // Controls
    float cy = 142, mid = w / 2;
    if (m.has_shuffle)
        w::icon_button(c, ID("music.shuffle"), {mid - 124, cy - 15, 30, 30}, m.shuffle ? "󰒟" : "󰒞", 14, false,
                       m.shuffle ? acc : p.text_faint)
            .click([this] { mpris_.toggle_shuffle(); });
    w::icon_button(c, ID("music.prev"), {mid - 72, cy - 17, 34, 34}, "󰒮", 18).click([this] { mpris_.previous(); });
    {
        uint64_t id = ID("music.play");
        Rect r{mid - 22, cy - 22, 44, 44};
        float s = w::press_scale(ui_, id, 0.12f);
        float hv = ui_.hover(id);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s);
        fill(cr, r, 22, mix(acc, Color{1, 1, 1, 1}, 0.1f * hv));
        Color on = acc.luminance() > 0.55f ? Color{0.05f, 0.05f, 0.07f, 1} : Color{1, 1, 1, 1};
        glyph(cr, r.cx() + (m.playing ? 0 : 1.5f), r.cy(), m.playing ? "󰏤" : "󰐊", 20, on);
        cairo_restore(cr);
        ui_.add(id, r).click([this] { mpris_.play_pause(); });
    }
    w::icon_button(c, ID("music.next"), {mid + 38, cy - 17, 34, 34}, "󰒭", 18).click([this] { mpris_.next(); });
    if (m.has_loop)
        w::icon_button(c, ID("music.loop"), {mid + 94, cy - 15, 30, 30},
                       m.loop == "Track" ? "󰑘" : "󰑖", 14, false, m.loop == "None" ? p.text_faint : acc)
            .click([this] { mpris_.cycle_loop(); });
}

// ── notification ───────────────────────────────────────────────────────────

std::string App::notif_icon_key(const Notification& n) const { return n.image; }

void App::activate_notification(const NotificationPtr& n) {
    if (!n) return;
    if (n->on_action) {
        notifs_.invoke(n->id, "default");
    } else if (n->live && n->has_default) {
        notifs_.invoke(n->id, "default");
    } else {
        std::string hint = !n->desktop_entry.empty() ? n->desktop_entry : n->app;
        niri_.focus_app(hint);
        if (n->live) notifs_.close(n->id);
        else notifs_.dismiss_current();
    }
}

void App::draw_notification(cairo_t* cr, float w, float h) {
    const auto& p = T();
    auto n = notifs_.current();
    if (!n) return;
    w::Ctx c{cr, ui_};
    bool critical = n->urgency >= 2;

    // Picture: the sender's pixels, else its image/icon, else a bell.
    Rect pic{14, 15, 48, 48};
    cairo_surface_t* img = n->pixels.get();
    if (!img && !n->image.empty()) img = ImageCache::get().request(n->image, 96);
    fill(cr, pic, 24, p.surface_hi);
    if (img) {
        bool is_icon = starts_with(n->image, "icon:") && !n->pixels;
        if (is_icon) image(cr, img, pic.inset(9), 4);
        else image(cr, img, pic, 24);
    } else {
        glyph(cr, pic.cx(), pic.cy(), critical ? "󰀪" : "󰂚", 19, critical ? p.bad : p.accent);
    }
    if (critical) stroke(cr, pic.inset(-1), 25, p.bad, 2);

    bool inline_actions = n->actions.size() <= 2;
    size_t letters = 0;
    for (auto& a : n->actions) letters += a.second.size();
    if (letters > 18) inline_actions = false;

    // Inline buttons measured first so the text knows its room.
    float actions_w = 0;
    std::vector<float> widths;
    if (inline_actions) {
        for (auto& a : n->actions) {
            float bw = measure(a.second, {ui(12.5f, "SemiBold")}).w + 28;
            widths.push_back(bw);
            actions_w += bw + 8;
        }
    }
    float tx = 74, tw = w - tx - 16 - actions_w;

    std::string meta = n->app.empty() ? "ahora" : n->app + "  ·  ahora";
    Size ms = actions_w > 0 ? Size{} : measure(meta, {ui(11)});

    // Is there more body than two lines show? Then offer a chevron by the title.
    bool expandable = false;
    if (!n->body.empty()) {
        Size full = measure(n->body, Text{ui(12.5f), {}, Align::Left, tw, 0, true, true});
        Size two = measure(n->body, Text{ui(12.5f), {}, Align::Left, tw, 2, true, true});
        expandable = full.h > two.h + 2;
    }
    float chev = expandable ? 26 : 0;
    text(cr, tx, 15, n->summary, {ui(14, "SemiBold"), p.text, Align::Left, tw - ms.w - 10 - chev});
    if (actions_w <= 0) text(cr, w - 16, 17, meta, {ui(11), p.text_faint, Align::Right});
    if (expandable) {
        float cx = actions_w > 0 ? tx + tw - 22 : w - 16 - ms.w - 30;
        w::icon_button(c, ID("notif.more"), {cx, 12, 24, 22}, notif_expanded_ ? "󰅃" : "󰅀", 13).click([this] {
            notif_expanded_ = !notif_expanded_;
            request();
        });
    }

    float body_h = 0;
    if (!n->body.empty()) {
        Text bt{ui(12.5f), p.text_dim, Align::Left, tw, notif_expanded_ ? 0 : 2, true, true};
        // Pango limits lines per paragraph: collapsed, the body must be a single
        // paragraph or every line break would add two more lines past the pill.
        std::string body = notif_expanded_ ? n->body : replace_all(replace_all(n->body, "\r", ""), "\n", "  ");
        cairo_save(cr);
        cairo_rectangle(cr, 0, 0, w, h - 6);
        cairo_clip(cr);
        body_h = text(cr, tx, 36, body, bt).h;
        cairo_restore(cr);
    }
    (void)body_h;

    // Buttons: inline at the end of the row, or a row of their own beneath.
    if (!n->actions.empty()) {
        uint32_t id = n->id;
        if (inline_actions) {
            float x = w - 14;
            for (size_t i = n->actions.size(); i-- > 0;) {
                x -= widths[i];
                std::string key = n->actions[i].first;
                w::pill(c, ID("notif.act", static_cast<int>(i)), {x, 24, widths[i], 30}, n->actions[i].second, nullptr,
                        i == 0 ? w::Style::Accent : w::Style::Plain)
                    .click([this, id, key] {
                        if (!notifs_.invoke(id, key)) notifs_.dismiss_current();
                    });
                x -= 8;
            }
        } else {
            float y = h - 42 - (n->value >= 0 ? 14 : 0);
            float bw = (w - 28 - 8 * (n->actions.size() - 1)) / n->actions.size();
            for (size_t i = 0; i < n->actions.size(); ++i) {
                std::string key = n->actions[i].first;
                w::pill(c, ID("notif.act", static_cast<int>(i)), {14 + i * (bw + 8), y, bw, 32}, n->actions[i].second,
                        nullptr, i == 0 ? w::Style::Accent : w::Style::Plain)
                    .click([this, id, key] {
                        if (!notifs_.invoke(id, key)) notifs_.dismiss_current();
                    });
            }
        }
    }
    if (n->value >= 0) bar(cr, {tx, h - 16, w - tx - 16, 5}, n->value / 100.0f, p.accent, p.text.with_alpha(0.12f));

    // Time left, as a hairline along the bottom edge.
    int timeout = n->urgency >= 2 ? 0 : (n->timeout_ms > 0 ? std::min(n->timeout_ms, 15000)
                                                           : n->timeout_ms == 0 ? 15000 : config().notification_timeout_ms);
    if (timeout > 0) {
        // Time left, edge to edge inside the pill's rounded ends, shrinking to the centre.
        float left = 1.0f - clamp01(static_cast<float>((now_mono() - notif_shown_at_) * 1000.0 / timeout));
        float inset = std::min(h / 2.0f, 28.0f) * 0.75f;
        float full = w - 2 * inset;
        fill(cr, {inset + full * (1 - left) / 2, h - 3.5f, full * left, 2.5f}, 1.25f, p.accent.with_alpha(0.6f));
    }

    // Close on hover.
    uint64_t xid = ID("notif.close");
    if (ui_.inside() && island_rect().contains(ui_.px(), ui_.py())) {
        w::icon_button(c, xid, {pic.right() - 14, pic.y - 6, 20, 20}, "󰅖", 11, false, p.text)
            .click([this, id = n->id] { notifs_.close(id); });
    }
}

// ── OSD ────────────────────────────────────────────────────────────────────

void App::draw_osd(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const OsdInfo& o = osd_;
    auto keep_open = [this] {
        osd_.until = now_mono() + 2.2;
        loop_.cancel(osd_timer_);
        osd_timer_ = loop_.after(2.22, [this] {
            osd_timer_ = 0;
            request();
        });
    };

    if (o.kind == OsdInfo::Battery) {
        // Ring with the cell inside, the figure, then power and charge.
        const auto& b = power_.battery();
        Color col = p.accent;  // the theme's colour, charging or not
        float cx = 40, cy = 40;
        ring(cr, cx, cy, 24, 4.5f, b.percent / 100.0f, col, p.text.with_alpha(0.1f));
        glyph(cr, cx, cy, b.charging ? "󰂄" : b.percent > 80 ? "󰁹" : b.percent > 50 ? "󰂀" : b.percent > 20 ? "󰁽" : "󰁻",
              17, col);
        text(cr, 78, 14, std::to_string(b.percent) + "%", {ui(22, "Bold"), p.text});
        std::string sub = b.charging ? "Cargando" : b.plugged ? "Conectado" : "Con batería";
        if (b.minutes > 0) {
            int hh = b.minutes / 60, mm = b.minutes % 60;
            sub += "  ·  " + (hh ? std::to_string(hh) + " h " : "") + std::to_string(mm) + " min " +
                   (b.charging ? "hasta llenar" : "restantes");
        } else if (b.status == "Full") {
            sub += "  ·  carga completa";
        }
        text(cr, 78, 44, sub, {ui(12), p.text_dim, Align::Left, w - 92});
        float colw = (w - 36) / 2;
        char pw[32], ch[32], full[48];
        snprintf(pw, sizeof(pw), "%.1f W", b.watts);
        snprintf(ch, sizeof(ch), "%.1f Wh", b.energy_wh);
        snprintf(full, sizeof(full), "de %.1f Wh", b.energy_full_wh);
        text(cr, 18, 78, "POTENCIA", {ui(10.5f, "Bold"), p.text_faint});
        text(cr, 18, 92, pw, {mono(18, "Bold"), p.text});
        text(cr, 18, 116, b.charging ? "entrando" : b.watts > 0.1f ? "saliendo" : "en reposo", {ui(11), p.text_dim});
        text(cr, 18 + colw, 78, "CARGA", {ui(10.5f, "Bold"), p.text_faint});
        text(cr, 18 + colw, 92, ch, {mono(18, "Bold"), p.text});
        text(cr, 18 + colw, 116, full, {ui(11), p.text_dim});
        return;
    }

    if (o.kind == OsdInfo::Volume || o.kind == OsdInfo::Brightness) {
        bool vol = o.kind == OsdInfo::Volume;
        const auto& sink = audio_.state().sink;
        bool muted = vol && sink.muted;
        float v = vol ? std::min(1.0f, sink.volume) : power_.brightness();
        Color col = muted ? p.text_faint : p.accent;
        // The ring is a dial: drag around it, or scroll on it.
        float cx = 40, cy = vol ? 46 : 36;
        uint64_t rid = ID("osd.ring");
        float rh = ui_.hover(rid);
        ring(cr, cx, cy, 24 + rh, 4.5f, v, col, p.text.with_alpha(0.1f));
        const char* g = vol ? (muted || v < 0.005f ? "󰝟" : v < 0.34f ? "󰕿" : v < 0.67f ? "󰖀" : "󰕾")
                            : (v < 0.34f ? "󰃞" : v < 0.67f ? "󰃟" : "󰃠");
        glyph(cr, cx, cy, g, 17, muted ? p.text_dim : p.text);
        auto set_level = [this, vol](float x) {
            if (vol) audio_.set_volume(clamp01(x));
            else power_.set_brightness(clamp01(x));
        };
        ui_.add(rid, {cx - 30, cy - 30, 60, 60})
            .click([this, vol] {
                if (vol) audio_.toggle_mute();
            })
            .scroll([this, set_level, v, keep_open](float dy) {
                set_level(v - dy * 0.05f);
                keep_open();
            });

        float tx = 80, tw = w - tx - 18;
        text(cr, tx, vol ? 14 : 12, vol ? "Volumen" : "Brillo", {ui(14, "SemiBold"), p.text});
        std::string pct = muted ? "Silenciado" : std::to_string(static_cast<int>(std::lround(v * 100))) + "%";
        text(cr, w - 18, vol ? 15 : 13, pct, {ui(13, "SemiBold"), muted ? p.text_dim : p.text, Align::Right});
        Rect b{tx, vol ? 40.0f : 38.0f, tw, 6};
        uint64_t bid = ID("osd.bar");
        float hv = ui_.hover(bid);
        Rect bb = b.inset(0, -hv * 1.5f);
        bar(cr, bb, v, col, p.text.with_alpha(0.12f));
        fill(cr, {b.x + b.w * v - 6, b.cy() - 6, 12, 12}, 6, Color{1, 1, 1, 0.95f * (0.4f + 0.6f * hv)});
        ui_.add(bid, b.inset(0, -10)).drag([set_level, keep_open](const DragEvent& e) {
            set_level(e.fx);
            keep_open();
        });
        if (vol) {
            float pw = (tw - 8) / 2;
            bool mic_muted = audio_.state().source.muted;
            w::pill(c, ID("osd.mute"), {tx, 58, pw, 30}, muted ? "Activar" : "Silenciar", muted ? "󰕾" : "󰝟",
                    muted ? w::Style::Accent : w::Style::Plain, 12)
                .click([this, keep_open] {
                    audio_.toggle_mute();
                    keep_open();
                });
            w::pill(c, ID("osd.mic"), {tx + pw + 8, 58, pw, 30}, mic_muted ? "Mic apagado" : "Micrófono",
                    mic_muted ? "󰍭" : "󰍬", mic_muted ? w::Style::Plain : w::Style::Tonal, 12)
                .click([this, keep_open] {
                    audio_.toggle_mic_mute();
                    keep_open();
                });
        } else if (!o.detail.empty() && o.detail != "Brillo") {
            text(cr, tx, 52, o.detail, {ui(11), p.text_faint, Align::Left, tw});
        }
        return;
    }

    // Messages and the microphone.
    Rect ic{12, (h - 36) / 2, 36, 36};
    fill(cr, ic, 18, o.muted ? p.text.with_alpha(0.1f) : p.accent_dim);
    glyph(cr, ic.cx(), ic.cy(), o.glyph, 17, o.muted ? p.text_dim : p.accent);
    if (o.kind == OsdInfo::Mic) w::hit(c, ID("osd.mic1"), ic).click([this] { audio_.toggle_mic_mute(); });
    float ty = o.detail.empty() ? h / 2 - 9 : 12;
    text(cr, 60, ty, o.label, {ui(14, "SemiBold"), p.text, Align::Left, w - 76 - (o.value >= 0 ? 60 : 0)});
    if (!o.detail.empty()) text(cr, 60, 32, o.detail, {ui(11.5f), p.text_dim, Align::Left, w - 76});
    if (o.value >= 0) {
        ring(cr, w - 30, h / 2, 13, 3.5f, o.value, p.accent, p.text.with_alpha(0.1f));
        text(cr, w - 30, h / 2 - 6, std::to_string(static_cast<int>(std::lround(o.value * 100))),
             {ui(9.5f, "Bold"), p.text, Align::Center});
    }
}

// ── discreet notification ping ───────────────────────────────────────────

void App::draw_ping(cairo_t* cr) {
    Rect r = ping_rect();
    if (r.empty()) return;
    const auto& p = T();
    float a = clamp01(ping_alpha_.value);
    // A phone on the table: quick shakes that die out.
    float t = static_cast<float>(now_mono() - ping_at_);
    float shake = t < 0.9f ? std::sin(t * 55.0f) * 3.2f * (1.0f - t / 0.9f) : 0.0f;
    float rot = shake * 0.05f;
    cairo_save(cr);
    cairo_translate(cr, r.cx() + shake, r.cy());
    cairo_rotate(cr, rot);
    cairo_scale(cr, 0.6 + 0.4 * a, 0.6 + 0.4 * a);
    cairo_translate(cr, -r.cx(), -r.cy());
    cairo_push_group(cr);
    shadow_outside(cr, r, r.h / 2, 0.4f, 8);
    fill(cr, r, r.h / 2, glass_body());
    cairo_surface_t* img = ping_pixels_ ? ping_pixels_.get() : ping_icon_.empty() ? nullptr : ImageCache::get().request(ping_icon_, 48);
    if (img) image(cr, img, r.inset(6), ping_pixels_ ? 9 : 2);
    else glyph(cr, r.cx(), r.cy(), "󰂚", 14, p.accent);
    // A small accent dot: something arrived.
    fill(cr, {r.right() - 8, r.y, 8, 8}, 4, p.accent);
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, a);
    cairo_restore(cr);
    ui_.add(ID("ping"), r).click([this] {
        ping_alpha_.set(0);
        open(View::Control);
    });
    if (t < 1.0f) request();
}

// ── workspaces bubble ──────────────────────────────────────────────────────

}  // namespace island
