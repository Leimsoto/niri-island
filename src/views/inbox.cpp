// Two panels that drop from bar modules: the notifications inbox (with Do
// Not Disturb) and the system tray (apps' icons and their menus).

#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

// ── notifications ──────────────────────────────────────────────────────────

void App::draw_inbox(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& hist = notifs_.history();
    w::header(c, ID("in.back"), w, "Notificaciones", hist.empty() ? "Nada nuevo" : std::to_string(hist.size()) + " guardadas")
        .click([this] { back(); });
    if (!hist.empty())
        w::pill(c, ID("in.clear"), {w - 16 - 110, 15, 110, 30}, "Borrar todo", "󰎟", w::Style::Plain, 12).click([this] {
            notifs_.clear_history();
            inbox_scroll_.reset(0);
        });

    // Do not disturb: three modes, side by side
    const char* modes[3] = {"Avisos", "Discreto", "No molestar"};
    const char* glyphs[3] = {"󰂚", "󱏧", "󰂛"};
    float mw = (w - 32 - 2 * 6) / 3;
    for (int i = 0; i < 3; ++i) {
        bool on = notifs_.dnd_mode() == i;
        w::pill(c, ID("in.dnd", i), {16 + i * (mw + 6), 58, mw, 32}, modes[i], glyphs[i],
                on ? (i == 2 ? w::Style::Danger : w::Style::Accent) : w::Style::Plain, 12)
            .click([this, i] { notifs_.set_dnd_mode(i); });
    }

    Rect list{14, 102, w - 28, h - 102 - 12};
    if (hist.empty()) {
        glyph(cr, list.cx(), list.cy() - 14, "󰂜", 30, p.text_faint);
        text(cr, list.cx(), list.cy() + 10, "Nada por aquí", {ui(12.5f), p.text_faint, Align::Center});
        return;
    }
    float prev_content = inbox_content_;
    ui_.add(ID("in.scroll"), list).scroll([this, list, prev_content](float dy) {
        inbox_scroll_.set(std::clamp(inbox_scroll_.target + dy * 52, 0.0f, std::max(0.0f, prev_content - list.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, list.x - 2, list.y, list.w + 4, list.h);
    cairo_clip(cr);
    ui_.push_clip(list);
    float yy = list.y - inbox_scroll_.value;
    for (const auto& n : hist) {
        Text bt{ui(11.5f), p.text_dim, Align::Left, list.w - 60, 2, true, true};
        Size bs = n->body.empty() ? Size{} : measure(n->body, bt);
        float ch = 42 + bs.h + 8;
        Rect card{list.x, yy, list.w, ch};
        if (card.bottom() >= list.y && card.y <= list.bottom()) {
            uint64_t id = ID("in.n", n->id);
            float hv = ui_.hover(id);
            fill(cr, card, 14, mix(p.surface_hi.fade(0.6f), p.surface_hi, hv));
            Rect ic{card.x + 10, card.y + 10, 26, 26};
            cairo_surface_t* img = n->pixels.get();
            if (!img && !n->image.empty()) img = ImageCache::get().request(n->image, 48);
            if (img) image(cr, img, ic, starts_with(n->image, "icon:") && !n->pixels ? 4 : 13);
            else glyph(cr, ic.cx(), ic.cy(), n->urgency >= 2 ? "󰀪" : "󰂚", 14, n->urgency >= 2 ? p.bad : p.accent);
            std::string meta = (n->app.empty() ? "" : n->app + " · ") + time_ago(n->time);
            text(cr, card.x + 46, card.y + 8, meta, {ui(10.5f), p.text_faint, Align::Left, card.w - 84});
            text(cr, card.x + 46, card.y + 23, n->summary, {ui(13, "SemiBold"), p.text, Align::Left, card.w - 64});
            if (!n->body.empty()) text(cr, card.x + 46, card.y + 42, n->body, bt);
            ui_.add(id, card).click([this, n] {
                if (n->live || n->on_action) activate_notification(n);
                else niri_.focus_app(n->desktop_entry.empty() ? n->app : n->desktop_entry);
                close_all();
            });
            if (hv > 0.05f)
                w::icon_button(c, ID("in.x", n->id), {card.right() - 30, card.y + 6, 24, 24}, "󰅖", 11)
                    .click([this, nid = n->id] { notifs_.remove(nid); });
        }
        yy += ch + 8;
    }
    inbox_content_ = yy + inbox_scroll_.value - list.y;
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {list.right() + 4, list.y + 4, 3, list.h - 8}, inbox_scroll_.value, inbox_content_, list.h);
}

// ── system tray ───────────────────────────────────────────────────────────

cairo_surface_t* tray_icon(const TrayItem& it) {
    if (it.pixmap) return it.pixmap.get();
    if (!it.icon_name.empty()) {
        if (it.icon_name[0] == '/') return ImageCache::get().request(it.icon_name, 64);
        if (!it.theme_path.empty())
            for (const char* ext : {".png", ".svg"})
                if (file_exists(it.theme_path + "/" + it.icon_name + ext))
                    return ImageCache::get().request(it.theme_path + "/" + it.icon_name + ext, 64);
        return ImageCache::get().request("icon:" + it.icon_name, 64);
    }
    if (!it.id.empty()) return ImageCache::get().request("icon:" + lower(it.id), 64);
    return nullptr;
}

void App::open_tray_menu(const std::string& key) {
    tray_menu_pending_ = true;  // survives the view's reset on entering
    if (nav_.empty() || nav_.back() != View::Tray) open(View::Tray);
    tray_menu_key_ = key;
    tray_menu_.clear();
    tray_menu_path_.clear();
    tray_menu_loading_ = true;
    tray_.menu(key, [this, key](std::vector<TrayMenuItem> items) {
        if (tray_menu_key_ != key) return;
        tray_menu_loading_ = false;
        tray_menu_ = std::move(items);
        if (tray_menu_.empty()) {
            // no menu: the app may still answer the old ContextMenu call
            tray_.secondary(key);
            tray_menu_key_.clear();
            close_all();
        }
        request();
    });
    request();
}

void App::draw_tray(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    const auto& items = tray_.items();

    // An app's menu, opened from its icon.
    if (!tray_menu_key_.empty()) {
        const TrayItem* it = nullptr;
        for (auto& x : items)
            if (x.key == tray_menu_key_) it = &x;
        std::string title = it ? it->name : "Menú";
        const std::vector<TrayMenuItem>* level = &tray_menu_;
        for (int idx : tray_menu_path_)
            if (idx >= 0 && idx < static_cast<int>(level->size())) level = &(*level)[idx].children;
        w::header(c, ID("tr.mback"), w, title, tray_menu_loading_ ? "Cargando…" : "").click([this] {
            if (!tray_menu_path_.empty()) tray_menu_path_.pop_back();
            else tray_menu_key_.clear();
        });
        float y = 60;
        int i = 0;
        for (auto& m : *level) {
            if (!m.visible) {
                ++i;
                continue;
            }
            if (m.separator) {
                divider(cr, 24, y + 5, w - 24, y + 5, p.border);
                y += 11;
                ++i;
                continue;
            }
            Rect r{12, y, w - 24, 36};
            uint64_t id = ID("tr.m", i);
            float hv = m.enabled ? ui_.hover(id) : 0;
            if (hv > 0.01f) fill(cr, r, 10, p.text.with_alpha(0.08f * hv));
            float tx = r.x + 14;
            if (m.toggle) {
                glyph(cr, r.x + 18, r.cy(), m.toggle == 2 ? (m.checked ? "󰐾" : "󰄰") : (m.checked ? "󰄲" : "󰄱"), 14,
                      m.checked ? p.accent : p.text_dim);
                tx += 22;
            }
            text(cr, tx, r.cy() - 9, m.label, {ui(13), m.enabled ? p.text : p.text_faint, Align::Left, r.w - 50});
            if (!m.children.empty()) glyph(cr, r.right() - 14, r.cy(), "󰅂", 13, p.text_dim);
            if (m.enabled) {
                int idx = i, mid = m.id;
                bool sub = !m.children.empty();
                ui_.add(id, r).click([this, idx, mid, sub] {
                    if (sub) {
                        tray_menu_path_.push_back(idx);
                        return;
                    }
                    tray_.menu_click(tray_menu_key_, mid);
                    tray_menu_key_.clear();
                    close_all();
                });
            }
            y += 38;
            ++i;
        }
        return;
    }

    w::header(c, ID("tr.back"), w, "Bandeja del sistema",
              items.empty() ? "Ninguna app en segundo plano" : std::to_string(items.size()) + (items.size() == 1 ? " app en segundo plano" : " apps en segundo plano"))
        .click([this] { back(); });
    if (items.empty()) {
        glyph(cr, w / 2, h / 2 + 2, "󰀻", 28, p.text_faint);
        text(cr, w / 2, h / 2 + 24, "Las apps con icono de bandeja aparecerán aquí", {ui(11.5f), p.text_faint, Align::Center, w - 40});
        return;
    }
    // One row per app: click opens it, the ⋮ (or right click) shows its menu.
    float y = 62;
    for (size_t i = 0; i < items.size(); ++i) {
        const TrayItem& it = items[i];
        Rect r{12, y, w - 24, 48};
        if (r.y > h) break;
        int64_t hk = static_cast<int64_t>(std::hash<std::string>{}(it.key) & 0xffffff);
        uint64_t id = ID("tr.i", hk);
        float hv = ui_.hover(id);
        float s = w::press_scale(ui_, id, 0.02f);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s);
        fill(cr, r, 14, p.text.with_alpha(0.04f + 0.05f * hv));
        Rect ib{r.x + 8, r.y + 8, 32, 32};
        fill(cr, ib, 10, p.text.with_alpha(0.06f));
        Rect ic{ib.x + 6, ib.y + 6, 20, 20};
        if (cairo_surface_t* img = tray_icon(it)) image(cr, img, ic, 4);
        else glyph(cr, ib.cx(), ib.cy(), "󰣆", 15, p.text_dim);
        bool attention = it.status == "NeedsAttention";
        std::string name = it.name;
        if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        std::string sub = attention ? "Necesita tu atención" : it.item_is_menu ? "Clic para ver opciones" : "Clic para abrir · clic derecho, opciones";
        text(cr, r.x + 50, r.y + 7, name, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 100});
        text(cr, r.x + 50, r.y + 26, sub, {ui(10.5f), attention ? p.accent : p.text_faint, Align::Left, r.w - 100});
        cairo_restore(cr);
        std::string key = it.key;
        bool menu_only = it.item_is_menu;
        ui_.add(id, r)
            .click([this, key, menu_only] {
                if (menu_only) {
                    open_tray_menu(key);
                    return;
                }
                tray_.activate(key);
                close_all();
            })
            .right([this, key] { open_tray_menu(key); })
            .middle([this, key] { tray_.secondary(key); })
            .scroll([this, key](float dy) { tray_.scroll(key, dy > 0 ? -1 : 1); });
        if (!it.menu_path.empty() || menu_only)
            w::icon_button(c, ID("tr.dots", hk), {r.right() - 40, r.y + 9, 30, 30}, "󰇙", 14).click([this, key] { open_tray_menu(key); });
        y += 54;
    }
}

}  // namespace island
