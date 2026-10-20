// Calendar with tasks, the date picker, and the kanban board.

#include "island/app.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

static void shown_month(int offset, int& y, int& m0) {
    struct tm now = local_tm();
    int raw = now.tm_mon + offset;
    y = now.tm_year + 1900 + (raw >= 0 ? raw / 12 : (raw - 11) / 12);
    m0 = ((raw % 12) + 12) % 12;
}

static const char* kStatus[] = {"Por hacer", "En curso", "Hecho"};

void App::submit_task_input() {
    std::string title = trim(task_field_.text);
    if (!title.empty()) {
        int y, m0;
        shown_month(cal_offset_, y, m0);
        std::string date = cal_day_ > 0 ? date_iso(y, m0, cal_day_) : Store::today();
        store_.add_task(title, date);
    }
    task_field_.clear();
    task_input_ = false;
    update_keyboard();
    request();
}

void App::submit_board_input() {
    std::string title = trim(board_field_.text);
    if (!title.empty()) store_.add_task(title, Store::today());
    board_field_.clear();
    board_input_ = false;
    update_keyboard();
    request();
}

void App::open_picker(int task_id) {
    Task* t = store_.task(task_id);
    if (!t) return;
    picker_task_ = task_id;
    picker_offset_ = 0;
    int y = 0, m = 0, d = 0;
    if (sscanf(t->date.c_str(), "%d-%d-%d", &y, &m, &d) == 3) {
        picker_y_ = y;
        picker_m_ = m - 1;
        picker_d_ = d;
        struct tm now = local_tm();
        picker_offset_ = (y - (now.tm_year + 1900)) * 12 + (picker_m_ - now.tm_mon);
    } else {
        struct tm now = local_tm();
        picker_y_ = now.tm_year + 1900;
        picker_m_ = now.tm_mon;
        picker_d_ = now.tm_mday;
    }
    open(View::DatePicker);
}

// ── calendar ───────────────────────────────────────────────────────────────

void App::draw_calendar(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    w::header(c, ID("cal.back"), w, "Calendario", "Tareas y fechas").click([this] { back(); });

    Rect left{16, 62, 360, h - 78};
    w::card(c, left, 18);
    int y, m0;
    shown_month(cal_offset_, y, m0);
    draw_month(cr, left.inset(16, 16), cal_offset_, cal_day_, true, "cal.month",
               [this](int, int, int d) {
                   cal_day_ = cal_day_ == d ? -1 : d;
                   task_scroll_.reset(0);
                   request();
               },
               [this](int delta) {
                   cal_offset_ += delta;
                   cal_day_ = -1;
                   request();
               });

    Rect right{left.right() + 12, 62, w - left.right() - 28, h - 78};
    w::card(c, right, 18);
    std::string sel_date = cal_day_ > 0 ? date_iso(y, m0, cal_day_) : "";
    std::string title = cal_day_ > 0 ? Store::due_label(sel_date) : "Próximas";
    if (!title.empty()) title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
    text(cr, right.x + 16, right.y + 16, title, {ui(15, "Bold"), p.text, Align::Left, right.w - 200});
    w::pill(c, ID("cal.board"), {right.right() - 186, right.y + 12, 80, 30}, "Tablero", "󰄲", w::Style::Plain, 12)
        .click([this] { open(View::Board); });
    w::pill(c, ID("cal.new"), {right.right() - 98, right.y + 12, 84, 30}, "Nueva", "󰐕", w::Style::Accent, 12).click([this] {
        task_input_ = !task_input_;
        task_field_.clear();
        update_keyboard();
    });

    float ly = right.y + 54;
    if (task_input_) {
        Rect f{right.x + 14, ly, right.w - 28, 40};
        draw_text_field(cr, ID("cal.field"), f, task_field_, "Nueva tarea para " + (cal_day_ > 0 ? title : std::string("hoy")) + "…",
                        true);
        ly += 50;
    }

    // Filter: the selected day, or everything not done (then recent done).
    std::vector<const Task*> list;
    for (auto& t : store_.tasks()) {
        if (cal_day_ > 0 ? t.date == sel_date : !t.done()) list.push_back(&t);
    }
    if (cal_day_ <= 0)
        for (auto& t : store_.tasks())
            if (t.done()) list.push_back(&t);

    Rect lr{right.x + 10, ly, right.w - 20, right.bottom() - ly - 10};
    if (list.empty()) {
        glyph(cr, lr.cx(), lr.cy() - 24, "󰄬", 30, p.text_faint);
        text(cr, lr.cx(), lr.cy() + 2, cal_day_ > 0 ? "Nada para este día" : "Todo al día", {ui(13), p.text_dim, Align::Center});
        if (!task_input_)
            w::pill(c, ID("cal.empty.new"), {lr.cx() - 70, lr.cy() + 28, 140, 32}, "Añadir tarea", "󰐕", w::Style::Tonal)
                .click([this] {
                    task_input_ = true;
                    update_keyboard();
                });
        return;
    }
    float rowh = 46, gap = 4;
    float content = list.size() * (rowh + gap);
    ui_.add(ID("cal.list"), lr).scroll([this, content, lr](float dy) {
        task_scroll_.set(std::clamp(task_scroll_.target + dy * 46, 0.0f, std::max(0.0f, content - lr.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, lr.x - 2, lr.y, lr.w + 4, lr.h);
    cairo_clip(cr);
    ui_.push_clip(lr);
    float yy = lr.y - task_scroll_.value;
    std::string today = Store::today();
    for (const Task* t : list) {
        Rect r{lr.x, yy, lr.w, rowh};
        yy += rowh + gap;
        if (r.bottom() < lr.y || r.y > lr.bottom()) continue;
        int id = t->id;
        uint64_t rid = ID("cal.task", id);
        float hv = ui_.hover(rid);
        fill(cr, r, 12, p.text.with_alpha(0.05f * hv));
        ui_.add(rid, r).click([this, id] { open_picker(id); });
        // checkbox
        Rect cb{r.x + 10, r.cy() - 10, 20, 20};
        uint64_t cid = ID("cal.check", id);
        float chv = ui_.hover(cid);
        if (t->done()) {
            fill(cr, cb, 10, p.accent);
            glyph(cr, cb.cx(), cb.cy(), "󰄬", 12, p.on_accent);
        } else {
            stroke(cr, cb, 10, mix(p.text_faint, p.accent, chv), 1.6f);
        }
        ui_.add(cid, cb.inset(-6)).click([this, id, done = t->done()] { store_.set_task_status(id, done ? 0 : 2); });
        Color tc = t->done() ? p.text_faint : p.text;
        text(cr, r.x + 40, r.y + 6, t->title, {ui(13, "SemiBold"), tc, Align::Left, r.w - 120});
        if (t->done()) divider(cr, r.x + 40, r.y + 15, r.x + 40 + std::min(r.w - 120, measure(t->title, {ui(13, "SemiBold")}).w), r.y + 15, p.text_faint);
        bool overdue = !t->done() && t->date < today;
        std::string due = Store::due_label(t->date) + (t->status == 1 ? "  ·  en curso" : "");
        text(cr, r.x + 40, r.y + 25, due, {ui(11), overdue ? p.warn : p.text_faint, Align::Left, r.w - 120});
        if (hv > 0.05f || ui_.hover(ID("cal.del", id)) > 0.05f) {
            w::icon_button(c, ID("cal.date", id), {r.right() - 66, r.cy() - 14, 28, 28}, "󰃭", 13)
                .click([this, id] { open_picker(id); });
            w::icon_button(c, ID("cal.del", id), {r.right() - 34, r.cy() - 14, 28, 28}, "󰆴", 13, false, p.bad)
                .click([this, id] { store_.delete_task(id); });
        }
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {right.right() - 6, lr.y + 4, 3, lr.h - 8}, task_scroll_.value, content, lr.h);
}

// ── date picker ────────────────────────────────────────────────────────────

void App::draw_picker(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    Task* t = store_.task(picker_task_);
    w::header(c, ID("pick.back"), w, "Cambiar fecha", t ? t->title : "").click([this] { back(); });
    draw_month(cr, {20, 66, w - 40, 250}, picker_offset_, -1, true, "pick.month",
               [this](int y, int m0, int d) {
                   picker_y_ = y;
                   picker_m_ = m0;
                   picker_d_ = d;
                   request();
               },
               [this](int delta) {
                   picker_offset_ += delta;
                   request();
               },
               picker_y_, picker_m_, picker_d_);

    struct Quick {
        const char* label;
        int days;
    };
    Quick q[] = {{"Hoy", 0}, {"Mañana", 1}, {"+1 semana", 7}};
    float qw = (w - 40 - 16) / 3;
    for (int i = 0; i < 3; ++i) {
        int days = q[i].days;
        w::pill(c, ID("pick.q", i), {20 + i * (qw + 8), 324, qw, 30}, q[i].label, nullptr, w::Style::Plain, 12)
            .click([this, days] {
                struct tm t = local_tm(time(nullptr) + days * 86400);
                picker_y_ = t.tm_year + 1900;
                picker_m_ = t.tm_mon;
                picker_d_ = t.tm_mday;
                struct tm now = local_tm();
                picker_offset_ = (picker_y_ - (now.tm_year + 1900)) * 12 + (picker_m_ - now.tm_mon);
                request();
            });
    }
    float bw = (w - 40 - 10) / 2, by = h - 16 - 40;
    w::pill(c, ID("pick.cancel"), {20, by, bw, 40}, "Cancelar", nullptr, w::Style::Plain).click([this] { back(); });
    w::pill(c, ID("pick.save"), {20 + bw + 10, by, bw, 40}, "Guardar", "󰄬", w::Style::Accent).click([this] {
        if (picker_task_ >= 0) store_.set_task_date(picker_task_, date_iso(picker_y_, picker_m_, picker_d_));
        back();
    });
    (void)p;
}

// ── board ──────────────────────────────────────────────────────────────────

void App::draw_board(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};

    if (board_detail_ >= 0) {
        Task* t = store_.task(board_detail_);
        if (!t) {
            board_detail_ = -1;
        } else {
            int id = t->id;
            w::header(c, ID("bd.back"), w, t->title, Store::due_label(t->date)).click([this] {
                if (board_notes_focus_) store_.set_task_notes(board_detail_, board_notes_.text);
                board_notes_focus_ = false;
                board_detail_ = -1;
                update_keyboard();
            });
            // Status segmented control
            Rect seg{20, 70, 330, 40};
            fill(cr, seg, 20, p.surface_hi);
            float sw = (seg.w - 8) / 3;
            for (int i = 0; i < 3; ++i) {
                Rect r{seg.x + 4 + i * sw, seg.y + 4, sw, seg.h - 8};
                uint64_t sid = ID("bd.status", i);
                bool on = t->status == i;
                if (on) fill(cr, r, 16, p.accent);
                else if (ui_.hover(sid) > 0.01f) fill(cr, r, 16, p.text.with_alpha(0.06f * ui_.hover(sid)));
                text(cr, r.cx(), r.cy() - 8, kStatus[i], {ui(12.5f, "SemiBold"), on ? p.on_accent : p.text_dim, Align::Center});
                ui_.add(sid, r).click([this, id, i] { store_.set_task_status(id, i); });
            }
            w::pill(c, ID("bd.date"), {seg.right() + 12, 70, 150, 40}, Store::due_label(t->date), "󰃭", w::Style::Plain)
                .click([this, id] { open_picker(id); });
            w::pill(c, ID("bd.del"), {w - 20 - 110, 70, 110, 40}, "Eliminar", "󰆴", w::Style::Danger).click([this, id] {
                store_.delete_task(id);
                board_detail_ = -1;
            });

            Rect nr{20, 126, w - 40, h - 146};
            w::card(c, nr, 18);
            text(cr, nr.x + 16, nr.y + 14, "Notas", {ui(12, "Bold"), p.text_dim});
            if (!board_notes_focus_) board_notes_.set(t->notes);
            Text bt{ui(13.5f), p.text, Align::Left, nr.w - 32, 0, false, true};
            std::string body = board_notes_.text;
            if (body.empty() && !board_notes_focus_) {
                text(cr, nr.x + 16, nr.y + 40, "Escribe algo… (clic para editar)", {ui(13.5f), p.text_faint});
            } else {
                std::string shown = body;
                if (board_notes_focus_) {
                    size_t cpos = std::min(board_notes_.caret, body.size());
                    shown = body.substr(0, cpos) + "▏" + body.substr(cpos);
                }
                text(cr, nr.x + 16, nr.y + 40, shown, bt);
            }
            if (board_notes_focus_)
                text(cr, nr.right() - 16, nr.bottom() - 24, "Enter para guardar · Esc para cerrar", {ui(11), p.text_faint, Align::Right});
            ui_.add(ID("bd.notes"), nr).cursor_shape(CursorShape::Text).click([this] {
                if (!board_notes_focus_) {
                    board_notes_focus_ = true;
                    board_notes_.multiline = false;
                    board_notes_.caret = board_notes_.text.size();
                }
                update_keyboard();
            });
            return;
        }
    }

    w::header(c, ID("board.back"), w, "Tablero", std::to_string(store_.tasks().size()) + " tareas").click([this] { back(); });
    w::pill(c, ID("board.new"), {w - 20 - 130, 14, 130, 32}, "Nueva tarea", "󰐕", w::Style::Accent, 12).click([this] {
        board_input_ = !board_input_;
        board_field_.clear();
        update_keyboard();
    });

    float top = 60;
    if (board_input_) {
        draw_text_field(cr, ID("board.field"), {20, top, w - 40, 40}, board_field_, "Título de la tarea (Enter para crear)", true);
        top += 50;
    }
    float gap = 12, lw = (w - 40 - 2 * gap) / 3;
    Rect lanes[3];
    for (int l = 0; l < 3; ++l) lanes[l] = {20 + l * (lw + gap), top, lw, h - top - 16};

    // Which lane the dragged card is over.
    int over_lane = -1;
    if (drag_task_ >= 0 && drag_moved_) {
        float lx = ui_.local_x();
        for (int l = 0; l < 3; ++l)
            if (lx >= lanes[l].x && lx < lanes[l].right()) over_lane = l;
    }

    std::string today = Store::today();
    const Task* dragged = nullptr;
    for (int l = 0; l < 3; ++l) {
        Rect lr = lanes[l];
        fill(cr, lr, 18, over_lane == l ? mix(p.surface, p.accent_dim, 0.6f) : p.surface);
        stroke(cr, lr, 18, over_lane == l ? p.accent.with_alpha(0.5f) : p.border, 1);
        int count = 0;
        for (auto& t : store_.tasks()) count += t.status == l;
        Color dot = l == 0 ? p.text_dim : l == 1 ? p.warn : p.good;
        fill(cr, {lr.x + 16, lr.y + 20, 8, 8}, 4, dot);
        text(cr, lr.x + 32, lr.y + 14, kStatus[l], {ui(13, "Bold"), p.text});
        text(cr, lr.right() - 16, lr.y + 15, std::to_string(count), {ui(12, "SemiBold"), p.text_faint, Align::Right});

        float cy = lr.y + 46;
        for (auto& t : store_.tasks()) {
            if (t.status != l) continue;
            if (t.id == drag_task_ && drag_moved_) {
                dragged = &t;
                continue;
            }
            Text nt{ui(11.5f), p.text_dim, Align::Left, lr.w - 44, 2, false, true};
            Size ns = t.notes.empty() ? Size{} : measure(t.notes, nt);
            float ch = 50 + (t.notes.empty() ? 0 : ns.h + 4);
            Rect r{lr.x + 10, cy, lr.w - 20, ch};
            if (r.bottom() > lr.bottom() - 8) {
                text(cr, lr.cx(), lr.bottom() - 26, "…", {ui(14, "Bold"), p.text_faint, Align::Center});
                break;
            }
            cy += ch + 8;
            int id = t.id;
            uint64_t rid = ID("board.card", id);
            float hv = ui_.hover(rid);
            fill(cr, r, 14, mix(p.surface_hi, mix(p.surface_hi, p.text, 0.06f), hv));
            bool overdue = !t.done() && t.date < today;
            text(cr, r.x + 34, r.y + 8, t.title, {ui(13, "SemiBold"), t.done() ? p.text_faint : p.text, Align::Left, r.w - 46});
            text(cr, r.x + 34, r.y + 27, "󰃭 " + Store::due_label(t.date), {ui(11), overdue ? p.warn : p.text_faint});
            if (!t.notes.empty()) text(cr, r.x + 34, r.y + 46, t.notes, nt);
            // Card: drag to move between lanes, click to open.
            ui_.add(rid, r)
                .cursor_shape(CursorShape::Grab)
                .drag([this, id, r](const DragEvent& e) {
                    if (drag_task_ != id) {
                        drag_task_ = id;
                        drag_moved_ = false;
                        drag_x_ = e.x;
                        drag_y_ = e.y;
                        drag_dx_ = e.x - e.rect.x;
                        drag_dy_ = e.y - e.rect.y;
                    }
                    if (std::hypot(e.x - drag_x_, e.y - drag_y_) > 5) drag_moved_ = true;
                    request();
                })
                .release([this, id, split1 = lanes[1].x, split2 = lanes[2].x](const DragEvent& e) {
                    bool moved = drag_moved_;
                    drag_task_ = -1;
                    drag_moved_ = false;
                    if (!moved) {
                        board_detail_ = id;
                        board_notes_focus_ = false;
                        return;
                    }
                    float px = e.lx;
                    int lane = px < split1 ? 0 : px < split2 ? 1 : 2;
                    if (Task* tt = store_.task(id); tt && tt->status != lane) store_.set_task_status(id, lane);
                });
            // checkbox
            Rect cb{r.x + 9, r.y + 9, 18, 18};
            uint64_t cid = ID("board.check", id);
            if (t.done()) {
                fill(cr, cb, 9, p.good);
                glyph(cr, cb.cx(), cb.cy(), "󰄬", 11, p.on_accent);
            } else {
                stroke(cr, cb, 9, mix(p.text_faint, p.accent, ui_.hover(cid)), 1.5f);
            }
            ui_.add(cid, cb.inset(-5)).click([this, id, done = t.done()] { store_.set_task_status(id, done ? 0 : 2); });
        }
    }

    // The card being dragged follows the pointer.
    if (dragged) {
        float lx = ui_.local_x() - drag_dx_, ly = ui_.local_y() - drag_dy_;
        Rect r{lx, ly, lw - 20, 50};
        shadow(cr, r, 14, 0.6f, 12);
        fill(cr, r, 14, p.surface_hi);
        stroke(cr, r, 14, p.accent.with_alpha(0.6f), 1.5f);
        text(cr, r.x + 34, r.y + 8, dragged->title, {ui(13, "SemiBold"), p.text, Align::Left, r.w - 46});
        text(cr, r.x + 34, r.y + 27, "󰃭 " + Store::due_label(dragged->date), {ui(11), p.text_faint});
    }
}

}  // namespace island
