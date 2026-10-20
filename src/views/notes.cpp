// The note deck (pinned pastel papers) and the editor, where the island
// becomes the note's paper: the card grows out of its place in the deck and
// flies back into it, pin and all.

#include "island/app.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

static const uint32_t kPaper[] = {0xFEF6E4, 0xD8E2F3, 0xE4F5E1, 0xE3EEFF, 0xFBE3EA};
static const uint32_t kPaperDeep[] = {0xF2D9A2, 0xA9BDE3, 0xA9D9A1, 0xA9C6F5, 0xF2AFC1};
static const uint32_t kPins[] = {0xE5484D, 0x3E8EED, 0x30A46C, 0xF5B14C, 0x8E4EC6};
static const Color kInk = Color::hex(0x2B2A33);

// "[ ] " / "[x] " shown as ballot boxes; same byte length, so carets map 1:1.
static std::string display_body(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        bool line_start = i == 0 || s[i - 1] == '\n';
        if (line_start && s.compare(i, 3, "[ ]") == 0) {
            out += "\xE2\x98\x90";
            i += 3;
        } else if (line_start && (s.compare(i, 3, "[x]") == 0 || s.compare(i, 3, "[X]") == 0)) {
            out += "\xE2\x98\x91";
            i += 3;
        } else {
            out += s[i++];
        }
    }
    return out;
}

static Rect lerp_rect(const Rect& a, const Rect& b, float t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.w, b.w, t), lerp(a.h, b.h, t)};
}

// A push pin seen from above: shadow, glossy head.
static void draw_pin(cairo_t* cr, float cx, float cy, float scale, float alpha, Color col) {
    if (alpha <= 0.01f) return;
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_save(cr);
    cairo_translate(cr, 3, 5);
    cairo_scale(cr, 1.0, 0.6);
    cairo_arc(cr, 0, 0, 7.5, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.22 * alpha);
    cairo_fill(cr);
    cairo_arc(cr, 0, 0, 7, 0, 2 * M_PI);
    set(cr, col.fade(alpha));
    cairo_fill(cr);
    cairo_pattern_t* g = cairo_pattern_create_radial(-2.5, -2.5, 0.5, 0, 0, 7);
    cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.75 * alpha);
    cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
    cairo_arc(cr, 0, 0, 7, 0, 2 * M_PI);
    cairo_set_source(cr, g);
    cairo_fill(cr);
    cairo_pattern_destroy(g);
    cairo_restore(cr);
}

void App::open_note(int id) {
    flush_note();
    Note* n = store_.note(id);
    if (!n) return;
    note_id_ = id;
    note_title_.set(n->title);
    note_body_.multiline = true;
    note_body_.max_bytes = 64 * 1024;
    note_body_.set(n->content);
    note_focus_ = n->title.empty() ? 0 : 1;
    note_dirty_ = false;
    note_saved_at_ = 0;
    // Grow out of the card it was pinned as (or out of the "new" card).
    auto it = note_card_rects_.find(id);
    if (it == note_card_rects_.end()) it = note_card_rects_.find(-1);
    note_from_ = it != note_card_rects_.end() ? it->second : Rect{230, 180, 120, 120};
    note_expand_.reset(Spring::speed > 0 ? 0.0f : 1.0f);
    note_expand_.set(1.0f);
    open(View::NoteEditor);
    update_keyboard();
}

void App::flush_note() {
    loop_.cancel(note_save_timer_);
    note_save_timer_ = 0;
    if (note_id_ < 0) return;
    Note* n = store_.note(note_id_);
    if (!n) return;
    if (n->title != note_title_.text || n->content != note_body_.text) {
        store_.update_note(note_id_, note_title_.text, note_body_.text);
        note_saved_at_ = now_mono();
    }
    note_dirty_ = false;
    request();
}

void App::draw_notes(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    auto archived = store_.archived_notes();
    if (notes_archived_view_ && archived.empty()) notes_archived_view_ = false;
    const std::vector<Note>& notes = notes_archived_view_ ? archived : store_.notes();
    std::string sub = notes_archived_view_ ? "Archivadas · clic para devolverla al tablero"
                    : notes.empty()        ? "Ninguna aún"
                                           : std::to_string(notes.size()) + " notas";
    w::header(c, ID("notes.back"), w, notes_archived_view_ ? "Archivo" : "Notas", sub).click([this] {
        if (notes_archived_view_) notes_archived_view_ = false;
        else back();
    });
    if (!archived.empty() || notes_archived_view_) {
        std::string label = notes_archived_view_ ? "Volver" : "Archivadas · " + std::to_string(archived.size());
        w::pill(c, ID("notes.arch"), {w - 16 - 150, 16, 150, 30}, label, "󰀼", w::Style::Plain, 12).click([this] {
            notes_archived_view_ = !notes_archived_view_;
            notes_scroll_.reset(0);
        });
    }

    Rect area{16, 60, w - 32, h - 68};
    float gap = 14, cw = (area.w - 2 * gap) / 3, ch = 158;
    int total = static_cast<int>(notes.size()) + 1;
    int rows = (total + 2) / 3;
    float content = rows * (ch + gap) - gap + 10;
    bool collapsing = note_return_id_ >= 0 && note_collapse_.value > 0.01f;
    if (!collapsing) note_return_id_ = -1;
    ui_.add(ID("notes.area"), area).scroll([this, content, area](float dy) {
        notes_scroll_.set(std::clamp(notes_scroll_.target + dy * 60, 0.0f, std::max(0.0f, content - area.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, area.x - 8, area.y - 6, area.w + 16, area.h + 6);
    cairo_clip(cr);
    ui_.push_clip(area);
    Rect returning_slot{};
    for (int i = 0; i < total; ++i) {
        // Pins sit 8 px into each card, so the grid starts a little lower.
        if (i == 0 && notes_archived_view_) continue;
        int slot = notes_archived_view_ ? i - 1 : i;
        Rect r{area.x + (slot % 3) * (cw + gap), area.y + 8 + (slot / 3) * (ch + gap) - notes_scroll_.value, cw, ch};
        if (i == 0) {
            note_card_rects_[-1] = r;
            if (r.bottom() < area.y || r.y > area.bottom()) continue;
            uint64_t id = ID("notes.new");
            float hv = ui_.hover(id);
            float s = w::press_scale(ui_, id, 0.04f);
            cairo_save(cr);
            scale_around(cr, r.cx(), r.cy(), s);
            fill(cr, r, 16, p.text.with_alpha(0.04f + 0.04f * hv));
            double dash[] = {6, 5};
            cairo_set_dash(cr, dash, 2, 0);
            stroke(cr, r.inset(1), 15, p.text.with_alpha(0.25f + 0.2f * hv), 1.5f);
            cairo_set_dash(cr, nullptr, 0, 0);
            glyph(cr, r.cx(), r.cy() - 12, "󰐕", 24, mix(p.text_dim, p.accent, hv));
            text(cr, r.cx(), r.cy() + 10, "Nueva nota", {hand(22, "Bold"), p.text_dim, Align::Center});
            cairo_restore(cr);
            ui_.add(id, r).click([this] {
                int nid = store_.add_note("", "", static_cast<int>(store_.notes().size() % 5));
                if (nid >= 0) open_note(nid);
            });
            continue;
        }
        const Note& n = notes[i - 1];
        note_card_rects_[n.id] = r;
        if (collapsing && n.id == note_return_id_) {
            returning_slot = r;
            continue;  // drawn on top, in flight
        }
        if (r.bottom() < area.y - 10 || r.y > area.bottom()) continue;
        uint64_t id = ID("notes.card", n.id);
        float hv = ui_.hover(id);
        float s = w::press_scale(ui_, id, 0.04f);
        int col = std::clamp(n.color, 0, 4);
        cairo_save(cr);
        scale_around(cr, r.cx(), r.cy(), s * (1.0f + 0.02f * hv));
        // A slight tilt, like paper pinned to a board; it straightens under the pointer.
        float tilt = ((n.id * 37) % 5 - 2) * 0.008f * (1.0f - hv);
        cairo_translate(cr, r.cx(), r.y + 8);
        cairo_rotate(cr, tilt);
        cairo_translate(cr, -r.cx(), -(r.y + 8));
        shadow(cr, r, 14, 0.35f + 0.25f * hv, 8 + 4 * hv);
        fill(cr, r, 14, Color::hex(kPaper[col]));
        std::string title = n.title.empty() ? "Sin título" : n.title;
        text(cr, r.x + 14, r.y + 18, title, {hand(24, "Bold"), kInk, Align::Left, r.w - 28});
        text(cr, r.x + 14, r.y + 50, display_body(n.content), {hand(18), kInk.with_alpha(0.8f), Align::Left, r.w - 28, 4, false, true});
        text(cr, r.x + 14, r.bottom() - 22, time_ago(n.updated), {ui(10.5f), kInk.with_alpha(0.45f)});
        draw_pin(cr, r.cx(), r.y + 8, 1.0f + 0.08f * hv, 1.0f, Color::hex(kPins[n.id % 5]));
        cairo_restore(cr);
        int nid = n.id;
        ui_.add(id, r).click([this, nid] {
            if (notes_archived_view_) {
                store_.unarchive_note(nid);
                notes_archived_view_ = false;
                show_toast("Nota devuelta al tablero");
                return;
            }
            open_note(nid);
        });
    }
    ui_.pop_clip();
    cairo_restore(cr);

    // The note just left, flying back to its slot; its pin drops in last.
    if (collapsing) {
        if (Note* n = store_.note(note_return_id_)) {
            float t = clamp01(note_collapse_.value);
            Rect full{0, 0, w, std::max(h, 480.0f)};
            Rect slot = returning_slot.empty() ? note_card_rects_[note_return_id_] : returning_slot;
            Rect r = lerp_rect(slot, full, t);
            int col = std::clamp(n->color, 0, 4);
            shadow(cr, r, lerp(14, 28, t), 0.5f, 12);
            fill(cr, r, lerp(14, 28, t), Color::hex(kPaper[col]));
            float a = 1.0f - smoothstep(t / 0.5f);
            if (a > 0.01f) {
                cairo_push_group(cr);
                text(cr, r.x + 14, r.y + 18, n->title.empty() ? "Sin título" : n->title,
                     {hand(24, "Bold"), kInk, Align::Left, r.w - 28});
                text(cr, r.x + 14, r.y + 50, display_body(n->content),
                     {hand(18), kInk.with_alpha(0.8f), Align::Left, r.w - 28, 4, false, true});
                cairo_pop_group_to_source(cr);
                cairo_paint_with_alpha(cr, a);
            }
            float drop = clamp01(1.0f - t / 0.22f);  // the last stretch of the flight
            draw_pin(cr, r.cx(), r.y + 8, 1.0f + (1.0f - drop) * 0.9f, drop, Color::hex(kPins[n->id % 5]));
        }
    }
    w::scrollbar(c, {w - 9, area.y + 4, 3, area.h - 8}, notes_scroll_.value, content, area.h);
}

void App::draw_note_editor(cairo_t* cr, float w, float h) {
    w::Ctx c{cr, ui_};
    Note* n = store_.note(note_id_);
    if (!n) return;
    int col = std::clamp(n->color, 0, 4);
    Color ink = kInk;
    Color faint = kInk.with_alpha(0.4f);
    float e = std::clamp(note_expand_.value, 0.0f, 1.04f);

    // Growing out of its card: paper first, then the writing fades in.
    float content_a = 1.0f;
    bool grouped = false;
    if (e < 0.999f) {
        Rect full{0, 0, w, h};
        Rect r = lerp_rect(note_from_, full, std::min(e, 1.0f));
        shadow(cr, r, lerp(14, 28, e), 0.5f, 12);
        fill(cr, r, lerp(14, 28, e), Color::hex(kPaper[col]));
        // The pin pops out as the card lifts off the board.
        float pop = clamp01(e / 0.3f);
        draw_pin(cr, r.cx(), r.y + 8, 1.0f + pop * 0.8f, 1.0f - pop, Color::hex(kPins[n->id % 5]));
        content_a = smoothstep((e - 0.55f) / 0.45f);
        if (content_a <= 0.01f) return;
        cairo_save(cr);
        rrect(cr, r, lerp(14, 28, e));
        cairo_clip(cr);
        cairo_push_group(cr);
        grouped = true;  // always popped below, whatever content_a ends up being
    }

    // Title
    Rect tr{26, 18, w - 52, 40};
    Text tt{hand(32, "Bold"), ink};
    bool tf = note_focus_ == 0;
    if (note_title_.text.empty()) text(cr, tr.x, tr.y, "Título", {hand(32, "Bold"), faint});
    else text(cr, tr.x, tr.y, note_title_.text, {hand(32, "Bold"), ink, Align::Left, tr.w});
    if (tf && std::fmod(anim_time_, 1.1f) < 0.7f) {
        float cx = caret_x(note_title_.text, tt, note_title_.caret);
        fill(cr, {tr.x + cx, tr.y + 6, 2, 30}, 1, Color::hex(kPaperDeep[col]).with_alpha(1));
    }
    ui_.add(ID("ed.title"), tr).cursor_shape(CursorShape::Text).click([this, tr, tt] {
        note_focus_ = 0;
        note_title_.caret = index_at(note_title_.text, tt, ui_.click_x() - tr.x);
        update_keyboard();
    });
    divider(cr, 26, 64, w - 26, 64, kInk.with_alpha(0.1f));

    // Body: ruled like a notebook page
    Rect br{26, 72, w - 52, h - 72 - 70};
    float line_h = 27;
    for (float ly = br.y + line_h; ly < br.bottom(); ly += line_h)
        divider(cr, br.x, ly + 0.5f, br.right(), ly + 0.5f, Color::hex(kPaperDeep[col]).with_alpha(0.35f));
    Text bt{hand(22), ink, Align::Left, br.w, 0, false, true};
    std::string shown = display_body(note_body_.text);
    cairo_save(cr);
    cairo_rectangle(cr, br.x - 4, br.y, br.w + 8, br.h);
    cairo_clip(cr);
    Rect caret = caret_rect(shown, bt, note_body_.caret);
    float scroll = std::max(0.0f, caret.bottom() - br.h + 6);
    if (note_body_.text.empty())
        text(cr, br.x, br.y, "Escribe aquí…  ([ ] al inicio de una línea crea una casilla)", {hand(22), faint});
    else text(cr, br.x, br.y - scroll, shown, bt);
    if (note_focus_ == 1 && std::fmod(anim_time_, 1.1f) < 0.7f)
        fill(cr, {br.x + caret.x, br.y + caret.y - scroll, 2, caret.h}, 1, Color::hex(kPaperDeep[col]));
    cairo_restore(cr);
    ui_.add(ID("ed.body"), br).cursor_shape(CursorShape::Text).click([this, br, bt, scroll] {
        note_focus_ = 1;
        std::string disp = display_body(note_body_.text);
        size_t idx = index_at_xy(disp, bt, ui_.click_x() - br.x, ui_.click_y() - br.y + scroll);
        // A click on a box ticks it.
        size_t ls = idx == 0 ? 0 : note_body_.text.rfind('\n', idx - 1);
        ls = ls == std::string::npos ? 0 : (idx == 0 ? 0 : ls + 1);
        std::string& t = note_body_.text;
        if (idx <= ls + 3 && t.size() >= ls + 3 && t[ls] == '[' && t[ls + 2] == ']') {
            t[ls + 1] = (t[ls + 1] == ' ') ? 'x' : ' ';
            flush_note();
        } else {
            note_body_.caret = idx;
        }
        update_keyboard();
    });

    // Bottom bar: back, paper colours, checklist, save state + Save, archive, delete.
    float by = h - 52;
    divider(cr, 20, by - 8, w - 20, by - 8, kInk.with_alpha(0.08f));
    w::icon_button(c, ID("ed.back"), {16, by, 36, 36}, "󰁍", 17, false, ink).click([this] { back(); });
    for (int i = 0; i < 5; ++i) {
        Rect dot{62 + i * 28.0f, by + 7, 22, 22};
        uint64_t id = ID("ed.color", i);
        float hv = ui_.hover(id);
        fill(cr, dot.inset(-hv * 2), 11 + hv * 2, Color::hex(kPaperDeep[i]));
        if (i == col) stroke(cr, dot.inset(-3), 14, ink.with_alpha(0.6f), 1.5f);
        int nid = n->id;
        ui_.add(id, dot.inset(-3)).click([this, nid, i] { store_.set_note_color(nid, i); });
    }
    w::icon_button(c, ID("ed.check"), {206, by, 36, 36}, "󰄲", 16, false, ink).click([this] {
        // Toggle a checkbox prefix on the caret's line.
        std::string& t = note_body_.text;
        size_t ls = note_body_.caret == 0 ? 0 : t.rfind('\n', note_body_.caret - 1);
        ls = ls == std::string::npos ? 0 : (note_body_.caret == 0 ? 0 : ls + 1);
        if (t.compare(ls, 4, "[ ] ") == 0 || t.compare(ls, 4, "[x] ") == 0) {
            t.erase(ls, 4);
            note_body_.caret = note_body_.caret >= ls + 4 ? note_body_.caret - 4 : ls;
        } else {
            t.insert(ls, "[ ] ");
            note_body_.caret += 4;
        }
        note_focus_ = 1;
        flush_note();
    });

    // Save state, in plain words, next to an explicit Save button.
    bool pending = note_dirty_ || note_save_timer_ != 0;
    bool just = !pending && note_saved_at_ > 0 && now_mono() - note_saved_at_ < 2.0;
    std::string state = pending ? "● Sin guardar" : just ? "✓ Guardado" : "✓ Guardado · " + time_ago(n->updated);
    Color sc = pending ? Color::hex(0xB86E00) : Color::hex(0x2F7D4F);
    text(cr, 254, by + 10, state, {ui(12, "SemiBold"), sc});
    Rect save{w - 200, by + 1, 96, 34};
    {
        uint64_t id = ID("ed.save");
        float hv = ui_.hover(id);
        float s = w::press_scale(ui_, id, 0.06f);
        cairo_save(cr);
        scale_around(cr, save.cx(), save.cy(), s);
        fill(cr, save, 17, mix(Color::hex(kPaperDeep[col]), ink, 0.15f + 0.15f * hv));
        glyph(cr, save.x + 22, save.cy(), "󰆓", 14, ink);
        text(cr, save.x + 36, save.cy() - 8, "Guardar", {ui(12.5f, "Bold"), ink});
        cairo_restore(cr);
        ui_.add(id, save).click([this] {
            flush_note();
            note_saved_at_ = now_mono();
            show_toast("Nota guardada");
        });
    }
    int nid = n->id;
    w::icon_button(c, ID("ed.archive"), {w - 96, by, 36, 36}, "󰀼", 16, false, ink.with_alpha(0.7f)).click([this, nid] {
        flush_note();
        store_.archive_note(nid);
        show_toast("Nota archivada (en «Archivadas»)");
        note_id_ = -1;
        note_return_id_ = -1;
        back();
    });
    w::icon_button(c, ID("ed.delete"), {w - 56, by, 36, 36}, "󰆴", 16, false, Color::hex(0xC0392B)).click([this, nid] {
        loop_.cancel(note_save_timer_);
        store_.delete_note(nid);
        note_id_ = -1;
        note_return_id_ = -1;
        back();
    });
    if (pending || just) request();

    if (grouped) {
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, content_a);
        cairo_restore(cr);
    }
}

}  // namespace island
