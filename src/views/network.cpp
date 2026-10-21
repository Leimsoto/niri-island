// Wi-Fi list, password prompt, network details (with a QR to share it) and
// the Bluetooth device list.

#include <qrencode.h>

#include "core/proc.hpp"
#include "island/app.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

static const char* signal_glyph(int s, bool secure) {
    (void)secure;
    if (s > 75) return "󰤨";
    if (s > 50) return "󰤥";
    if (s > 25) return "󰤢";
    return "󰤟";
}

static std::string band(const std::string& freq) {
    int f = std::atoi(freq.c_str());
    if (f >= 5900) return "6 GHz";
    if (f >= 4900) return "5 GHz";
    if (f > 0) return "2.4 GHz";
    return "";
}

void App::open_password(const std::string& ssid) {
    pass_ssid_ = ssid;
    pass_field_.clear();
    pass_visible_ = false;
    if (nav_.empty() || nav_.back() != View::WifiPassword) open(View::WifiPassword);
    update_keyboard();
    request();
}

void App::open_wifi_info(const WifiNetwork& net) {
    wifi_info_ = WifiDetails{};
    wifi_info_.net = net;
    wifi_qr_ = false;
    wifi_censor_ = true;
    open(View::WifiInfo);
    net_.load_details(net, [this](const WifiDetails& d) {
        if (d.net.ssid == wifi_info_.net.ssid) wifi_info_ = d;
        request();
    });
}

void App::draw_text_field(cairo_t* cr, uint64_t id, const Rect& r, TextField& f, const std::string& placeholder,
                          bool focused, bool secret, float font_px) {
    const auto& p = T();
    fill(cr, r, 12, p.surface_hi);
    stroke(cr, r, 12, focused ? p.accent.with_alpha(0.7f) : p.border, focused ? 1.5f : 1.0f);
    std::string shown = f.text;
    if (secret) {
        shown.clear();
        for (size_t i = 0; i < utf8_length(f.text); ++i) shown += "•";
    }
    Text t{ui(font_px), p.text};
    float tx = r.x + 12, ty = r.cy() - font_px * 0.62f;
    cairo_save(cr);
    cairo_rectangle(cr, r.x + 4, r.y, r.w - 8, r.h);
    cairo_clip(cr);
    // Keep the caret visible in long input: scroll the text left.
    size_t caret_bytes = f.caret;
    if (secret) caret_bytes = utf8_length(std::string_view(f.text).substr(0, f.caret)) * std::string("•").size();
    float cx = caret_x(shown, t, caret_bytes);
    float scroll = std::max(0.0f, cx - (r.w - 28));
    if (shown.empty()) text(cr, tx, ty, placeholder, {ui(font_px), p.text_faint});
    else text(cr, tx - scroll, ty, shown, t);
    if (focused) {
        float blink = std::fmod(anim_time_, 1.1f) < 0.7f ? 1.0f : 0.25f;
        fill(cr, {tx + cx - scroll, r.cy() - font_px * 0.6f, 1.6f, font_px * 1.2f}, 0.8f, p.accent.fade(blink));
    }
    cairo_restore(cr);
    ui_.add(id, r).cursor_shape(CursorShape::Text).click([this, &f, r, t, shown, scroll, secret] {
        if (!secret) f.caret = index_at(shown, t, ui_.click_x() - (r.x + 12) + scroll);
        else f.caret = f.text.size();
        request();
    });
}

// ── Wi-Fi ──────────────────────────────────────────────────────────────────

void App::draw_wifi(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& ns = net_.state();
    std::string sub = !ns.message.empty() ? ns.message
                    : !ns.wifi_enabled   ? "Desactivado"
                    : !ns.ssid.empty()   ? "Conectado a " + (demo_ ? std::string("Mi Wi-Fi") : ns.ssid)
                                         : "Sin conexión";
    w::header(c, ID("wifi.back"), w, "Wi-Fi", sub).click([this] { back(); });

    {
        uint64_t id = ID("wifi.scan");
        Rect r{w - 98, 14, 32, 32};
        float hv = ui_.hover(id);
        if (hv > 0.01f) fill(cr, r, 16, p.text.with_alpha(0.08f * hv));
        cairo_save(cr);
        if (ns.scanning) {
            cairo_translate(cr, r.cx(), r.cy());
            cairo_rotate(cr, anim_time_ * 5.0);
            cairo_translate(cr, -r.cx(), -r.cy());
        }
        glyph(cr, r.cx(), r.cy(), "󰑓", 15, ns.scanning ? p.accent : p.text_dim);
        cairo_restore(cr);
        ui_.add(id, r).click([this] { net_.scan(true); });
    }
    w::toggle(c, ID("wifi.toggle"), {w - 58, 18, 42, 24}, ns.wifi_enabled, [this] { net_.set_wifi(!net_.state().wifi_enabled); });

    Rect list{14, 62, w - 28, h - 62 - 14};
    if (!ns.wifi_enabled) {
        glyph(cr, list.cx(), list.cy() - 30, "󰤭", 40, p.text_faint);
        text(cr, list.cx(), list.cy() + 4, "El Wi-Fi está desactivado", {ui(13), p.text_dim, Align::Center});
        w::pill(c, ID("wifi.enable"), {list.cx() - 60, list.cy() + 30, 120, 34}, "Activar", "󰖩", w::Style::Accent)
            .click([this] { net_.set_wifi(true); });
        return;
    }
    if (ns.networks.empty()) {
        text(cr, list.cx(), list.cy() - 8, ns.scanning || ns.listing ? "Buscando redes…" : "No se encontraron redes",
             {ui(13), p.text_dim, Align::Center});
        return;
    }

    float rowh = 56, gap = 6;
    float content = ns.networks.size() * (rowh + gap);
    ui_.add(ID("wifi.list"), list).scroll([this, content, list](float dy) {
        wifi_scroll_.set(std::clamp(wifi_scroll_.target + dy * 50, 0.0f, std::max(0.0f, content - list.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, list.x - 4, list.y, list.w + 8, list.h);
    cairo_clip(cr);
    ui_.push_clip(list);
    float y = list.y - wifi_scroll_.value;
    int i = 0;
    for (const auto& n : ns.networks) {
        Rect r{list.x, y, list.w, rowh};
        y += rowh + gap;
        if (r.bottom() < list.y || r.y > list.bottom()) {
            ++i;
            continue;
        }
        bool connecting = ns.busy && ns.connecting_ssid == n.ssid;
        std::string s;
        if (connecting) s = "Conectando…";
        else if (n.in_use) s = "Conectado · " + band(n.freq);
        else if (n.saved) s = "Guardada" + (n.security.empty() ? std::string() : " · " + n.security);
        else s = n.security.empty() ? "Abierta" : "󰌾 " + n.security;
        WifiNetwork copy = n;
        w::row(c, ID("wifi.row", i), r, signal_glyph(n.signal, !n.security.empty()), n.ssid, s, n.in_use)
            .click([this, copy] {
                if (copy.in_use) open_wifi_info(copy);
                else if (copy.saved || copy.security.empty()) net_.connect(copy.ssid);
                else open_password(copy.ssid);
            })
            .right([this, copy] { open_wifi_info(copy); });
        w::icon_button(c, ID("wifi.info", i), {r.right() - 40, r.cy() - 15, 30, 30}, "󰋽", 14)
            .click([this, copy] { open_wifi_info(copy); });
        ++i;
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {w - 9, list.y + 4, 3, list.h - 8}, wifi_scroll_.value, content, list.h);
}

void App::draw_wifi_password(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& ns = net_.state();
    w::header(c, ID("pass.back"), w, pass_ssid_, "Introduce la contraseña").click([this] { back(); });
    Rect field{16, 64, w - 32, 44};
    draw_text_field(cr, ID("pass.field"), field, pass_field_, "Contraseña", true, !pass_visible_, 14);
    w::icon_button(c, ID("pass.eye"), {field.right() - 40, field.cy() - 15, 30, 30}, pass_visible_ ? "󰈈" : "󰈉", 15)
        .click([this] { pass_visible_ = !pass_visible_; });
    std::string msg = ns.busy ? "Conectando…" : ns.message;
    if (!pass_field_.text.empty() && pass_field_.text.size() < 8 && !ns.busy) msg = "Mínimo 8 caracteres";
    if (!msg.empty()) text(cr, 20, 116, msg, {ui(12), ns.busy ? p.text_dim : p.warn});
    float bw = (w - 32 - 10) / 2, by = h - 16 - 40;
    w::pill(c, ID("pass.cancel"), {16, by, bw, 40}, "Cancelar", nullptr, w::Style::Plain).click([this] { back(); });
    bool ok = pass_field_.text.size() >= 8 && !ns.busy;
    w::pill(c, ID("pass.ok"), {16 + bw + 10, by, bw, 40}, ns.busy ? "Conectando…" : "Conectar", "󰖩",
            ok ? w::Style::Accent : w::Style::Ghost)
        .click([this, ok] {
            if (ok) net_.connect(pass_ssid_, pass_field_.text);
        });
}

static void draw_qr(cairo_t* cr, const Rect& r, const std::string& data) {
    fill(cr, r, 14, Color{0.98f, 0.98f, 0.98f, 1});
    QRcode* q = QRcode_encodeString(data.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (!q) return;
    float m = 12, cell = (r.w - 2 * m) / q->width;
    cairo_set_source_rgb(cr, 0.06, 0.06, 0.08);
    for (int y = 0; y < q->width; ++y)
        for (int x = 0; x < q->width; ++x)
            if (q->data[y * q->width + x] & 1) cairo_rectangle(cr, r.x + m + x * cell, r.y + m + y * cell, cell + 0.4, cell + 0.4);
    cairo_fill(cr);
    QRcode_free(q);
}

static std::string qr_escape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        if (ch == '\\' || ch == ';' || ch == ',' || ch == ':' || ch == '"') out += '\\';
        out += ch;
    }
    return out;
}

void App::draw_wifi_info(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& d = wifi_info_;
    const auto& n = d.net;
    w::header(c, ID("winfo.back"), w, n.ssid, n.in_use ? "Conectado" : n.saved ? "Guardada" : "Disponible")
        .click([this] { back(); });
    w::icon_button(c, ID("winfo.eye"), {w - 48, 14, 32, 32}, wifi_censor_ ? "󰈉" : "󰈈", 15)
        .click([this] { wifi_censor_ = !wifi_censor_; });

    auto hide = [&](const std::string& v) {
        if (v.empty()) return std::string("—");
        return wifi_censor_ ? std::string("••••••••") : v;
    };
    struct Item {
        const char* label;
        std::string value;
    };
    std::vector<Item> items = {
        {"Señal", std::to_string(n.signal) + "%"},
        {"Seguridad", n.security.empty() ? "Abierta" : n.security},
        {"Banda", band(n.freq) + (n.channel.empty() ? "" : " · canal " + n.channel)},
        {"Velocidad", n.rate.empty() ? "—" : n.rate},
        {"Dirección IP", hide(d.ip)},
        {"Puerta de enlace", hide(d.gateway)},
        {"DNS", hide(d.dns)},
        {"MAC", hide(d.mac)},
    };
    Rect grid{16, 62, w - 32, 4 * 44.0f};
    w::card(c, grid, 16);
    float cw = grid.w / 2;
    for (size_t i = 0; i < items.size(); ++i) {
        float x = grid.x + 14 + (i % 2) * cw, y = grid.y + 8 + (i / 2) * 44.0f;
        text(cr, x, y, items[i].label, {ui(11), p.text_faint});
        text(cr, x, y + 15, items[i].value, {ui(12.5f, "SemiBold"), p.text, Align::Left, cw - 22});
    }

    // Password + sharing
    float py = grid.bottom() + 10;
    Rect pr{16, py, w - 32, 50};
    w::card(c, pr, 16);
    text(cr, pr.x + 14, pr.y + 8, "Contraseña", {ui(11), p.text_faint});
    std::string pass = !d.loaded ? "Cargando…" : d.password.empty() ? "No disponible" : hide(d.password);
    text(cr, pr.x + 14, pr.y + 24, pass, {mono(13, "SemiBold"), p.text, Align::Left, pr.w - 110});
    if (!d.password.empty()) {
        w::icon_button(c, ID("winfo.copy"), {pr.right() - 80, pr.cy() - 15, 30, 30}, "󰆏", 14).click([this] {
            std::string pw = wifi_info_.password;
            loop_.async([pw] { proc::run_quiet({"wl-copy"}, &pw); });
            show_toast("Contraseña copiada");
        });
        w::icon_button(c, ID("winfo.qr"), {pr.right() - 44, pr.cy() - 15, 30, 30}, "󰐲", 14, wifi_qr_)
            .click([this] { wifi_qr_ = !wifi_qr_; });
    }
    float ay = pr.bottom() + 10;
    if (wifi_qr_ && !d.password.empty()) {
        Rect q{w / 2 - 85, ay, 170, 170};
        std::string sec = n.security.find("WPA") != std::string::npos ? "WPA" : n.security.empty() ? "nopass" : "WEP";
        draw_qr(cr, q, "WIFI:T:" + sec + ";S:" + qr_escape(n.ssid) + ";P:" + qr_escape(d.password) + ";;");
        ay = q.bottom() + 10;
    }
    if (toast_until_ > now_mono())
        text(cr, w / 2, ay - 2, toast_text_, {ui(11.5f), p.good, Align::Center});

    float by = h - 16 - 40;
    float bw = n.saved ? (w - 32 - 10) / 2 : w - 32;
    WifiNetwork copy = n;
    if (n.in_use) {
        w::pill(c, ID("winfo.action"), {16, by, bw, 40}, "Desconectar", "󰖪", w::Style::Plain)
            .click([this] {
                net_.disconnect();
                back();
            });
    } else {
        w::pill(c, ID("winfo.action"), {16, by, bw, 40}, "Conectar", "󰖩", w::Style::Accent).click([this, copy] {
            if (copy.saved || copy.security.empty()) {
                net_.connect(copy.ssid);
                back();
            } else {
                open_password(copy.ssid);
            }
        });
    }
    if (n.saved)
        w::pill(c, ID("winfo.forget"), {16 + bw + 10, by, bw, 40}, "Olvidar red", "󰆴", w::Style::Danger)
            .click([this, copy] {
                net_.forget(copy.ssid);
                back();
            });
}

// ── Bluetooth ──────────────────────────────────────────────────────────────

static const char* bt_glyph(const std::string& icon) {
    if (icon.find("headset") != std::string::npos || icon.find("headphone") != std::string::npos) return "󰋋";
    if (icon.find("audio") != std::string::npos) return "󰓃";
    if (icon.find("mouse") != std::string::npos) return "󰍽";
    if (icon.find("keyboard") != std::string::npos) return "󰌌";
    if (icon.find("gaming") != std::string::npos) return "󰊗";
    if (icon.find("phone") != std::string::npos) return "󰏲";
    if (icon.find("computer") != std::string::npos) return "󰟀";
    if (icon.find("watch") != std::string::npos) return "󰖉";
    return "󰂯";
}

void App::draw_bluetooth(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& bs = bt_.state();
    std::string sub = !bs.message.empty() ? bs.message : !bs.available ? "Sin adaptador" : !bs.powered ? "Apagado"
                    : bs.discovering ? "Buscando dispositivos…" : "Activado";
    w::header(c, ID("bt.back"), w, "Bluetooth", sub).click([this] { back(); });
    if (!bs.available) {
        glyph(cr, w / 2, h / 2 - 20, "󰂲", 40, p.text_faint);
        text(cr, w / 2, h / 2 + 14, "No hay adaptador Bluetooth", {ui(13), p.text_dim, Align::Center});
        return;
    }
    {
        uint64_t id = ID("bt.scan");
        Rect r{w - 98, 14, 32, 32};
        float hv = ui_.hover(id);
        if (hv > 0.01f) fill(cr, r, 16, p.text.with_alpha(0.08f * hv));
        cairo_save(cr);
        if (bs.discovering) {
            cairo_translate(cr, r.cx(), r.cy());
            cairo_rotate(cr, anim_time_ * 5.0);
            cairo_translate(cr, -r.cx(), -r.cy());
        }
        glyph(cr, r.cx(), r.cy(), "󰑓", 15, bs.discovering ? p.accent : p.text_dim);
        cairo_restore(cr);
        ui_.add(id, r).click([this] {
            if (bt_.state().discovering) bt_.stop_discovery();
            else bt_.start_discovery();
        });
    }
    w::toggle(c, ID("bt.toggle"), {w - 58, 18, 42, 24}, bs.powered, [this] { bt_.set_powered(!bt_.state().powered); });

    Rect list{14, 62, w - 28, h - 76};
    if (!bs.powered) {
        glyph(cr, list.cx(), list.cy() - 30, "󰂲", 40, p.text_faint);
        w::pill(c, ID("bt.enable"), {list.cx() - 60, list.cy() + 10, 120, 34}, "Activar", "󰂯", w::Style::Accent)
            .click([this] { bt_.set_powered(true); });
        return;
    }
    if (bs.devices.empty()) {
        text(cr, list.cx(), list.cy() - 20, "No hay dispositivos", {ui(13), p.text_dim, Align::Center});
        w::pill(c, ID("bt.search"), {list.cx() - 70, list.cy() + 8, 140, 34}, "Buscar", "󰂰", w::Style::Tonal)
            .click([this] { bt_.start_discovery(); });
        return;
    }
    float rowh = 56, gap = 6;
    float content = bs.devices.size() * (rowh + gap);
    ui_.add(ID("bt.list"), list).scroll([this, content, list](float dy) {
        bt_scroll_.set(std::clamp(bt_scroll_.target + dy * 50, 0.0f, std::max(0.0f, content - list.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, list.x - 4, list.y, list.w + 8, list.h);
    cairo_clip(cr);
    ui_.push_clip(list);
    float y = list.y - bt_scroll_.value;
    int i = 0;
    for (const auto& d : bs.devices) {
        Rect r{list.x, y, list.w, rowh};
        y += rowh + gap;
        std::string s = d.busy ? "Un momento…" : d.connected ? "Conectado" : d.paired ? "Emparejado" : "Disponible";
        if (d.connected && d.battery >= 0) s += " · 󰁹 " + std::to_string(d.battery) + "%";
        std::string path = d.path;
        w::row(c, ID("bt.row", i), r, bt_glyph(d.icon), d.name, s, d.connected).click([this, path] { bt_.toggle_connect(path); });
        if (d.paired)
            w::icon_button(c, ID("bt.forget", i), {r.right() - 40, r.cy() - 15, 30, 30}, "󰆴", 13)
                .click([this, path] { bt_.forget(path); });
        ++i;
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {w - 9, list.y + 4, 3, list.h - 8}, bt_scroll_.value, content, list.h);
}

}  // namespace island
