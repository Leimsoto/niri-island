// Clipboard history (cliphist): a grid of what was copied, with real image
// thumbnails, colour swatches and links, a search and per-item actions.

#include <dirent.h>
#include <glib.h>
#include <sys/stat.h>
#include <unordered_set>

#include "core/config.hpp"
#include "core/proc.hpp"
#include "island/app.hpp"
#include "services/images.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"

namespace island {

using namespace paint;

static std::string clip_cache_dir() { return cache_dir() + "/clip"; }

// History can hold bytes that are not UTF-8 (binary copied as text).
static std::string valid_utf8(const std::string& s) {
    if (g_utf8_validate(s.data(), static_cast<gssize>(s.size()), nullptr)) return s;
    gchar* v = g_utf8_make_valid(s.data(), static_cast<gssize>(s.size()));
    std::string out = v ? v : "";
    g_free(v);
    return out;
}

static bool parse_hex_color(const std::string& s, Color& out) {
    std::string t = trim(s);
    if (t.size() != 7 && t.size() != 4) return false;
    if (t[0] != '#') return false;
    for (size_t i = 1; i < t.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(t[i]))) return false;
    if (t.size() == 4) t = std::string("#") + t[1] + t[1] + t[2] + t[2] + t[3] + t[3];
    out = Color::hex(static_cast<uint32_t>(std::stoul(t.substr(1), nullptr, 16)));
    return true;
}

void App::load_clipboard() {
    if (!proc::has("cliphist")) {
        clip_list_.clear();
        return;
    }
    clip_panel_loading_ = true;
    auto items = std::make_shared<std::vector<ClipItem>>();
    loop_.async(
        [items] {
            auto out = proc::run({"cliphist", "list"}, 4000).out;
            make_dirs(clip_cache_dir());
            int decoded = 0;
            for (auto& line : split(out, '\n')) {
                if (line.empty()) continue;
                if (items->size() >= 300) break;
                ClipItem it;
                it.line = line;
                size_t tab = line.find('\t');
                it.id = tab == std::string::npos ? "" : line.substr(0, tab);
                it.content = valid_utf8(tab == std::string::npos ? line : line.substr(tab + 1));
                std::string c = trim(it.content);
                // Images that came in through the text watcher show up as raw PNG/JPEG bytes.
                bool raw_png = c.size() > 8 && c.find("PNG") != std::string::npos && c.find("PNG") < 4 &&
                               c.find("IHDR") != std::string::npos;
                bool raw_jpg = c.find("JFIF") != std::string::npos && c.find("JFIF") < 12;
                if (raw_png || raw_jpg) c = std::string("[[ binary data ") + (raw_png ? "png" : "jpeg") + " ]]";
                if (c.find("[[ binary data") == 0) {
                    it.kind = 1;
                    // "[[ binary data 1 MiB png 1920x1080 ]]"
                    std::string m = c.substr(15);
                    if (m.size() > 3) m = m.substr(0, m.size() - 3);
                    it.meta = trim(m);
                    it.thumb = clip_cache_dir() + "/" + it.id + ".img";
                    // Decode the first images into the cache; the rest show a placeholder.
                    struct stat st {};
                    if (stat(it.thumb.c_str(), &st) != 0 && decoded < 48) {
                        auto r = proc::run({"cliphist", "decode"}, 4000, &line);
                        if (r.ok() && !r.out.empty()) write_file_atomic(it.thumb, r.out);
                        ++decoded;
                    }
                } else if (starts_with(c, "http://") || starts_with(c, "https://")) {
                    it.kind = 2;
                    std::string host = c.substr(c.find("//") + 2);
                    it.meta = host.substr(0, host.find('/'));
                } else if (parse_hex_color(c, it.color)) {
                    it.kind = 3;
                    it.meta = c;
                }
                items->push_back(std::move(it));
            }
            // Copies of images no longer in the history (deleted, wiped) go.
            std::unordered_set<std::string> keep;
            for (auto& it : *items)
                if (!it.thumb.empty()) keep.insert(it.id + ".img");
            if (!items->empty())
                if (DIR* d = opendir(clip_cache_dir().c_str())) {
                    while (dirent* e = readdir(d)) {
                        std::string f = e->d_name;
                        if (ends_with(f, ".img") && !keep.count(f)) unlink((clip_cache_dir() + "/" + f).c_str());
                    }
                    closedir(d);
                }
        },
        [this, items] {
            clip_panel_loading_ = false;
            clip_list_ = std::move(*items);
            request();
        });
}

void App::clip_copy(const ClipItem& it) {
    std::string line = it.line;
    loop_.async([line] {
        auto r = proc::run({"cliphist", "decode"}, 4000, &line);
        if (r.ok()) proc::run_quiet({"wl-copy"}, &r.out);
    });
    show_toast(it.kind == 1 ? "Imagen copiada" : "Copiado al portapapeles");
}

void App::clip_delete(const ClipItem& it) {
    std::string line = it.line, thumb = it.thumb;
    loop_.async([line, thumb] {
        proc::run({"cliphist", "delete"}, 4000, &line);
        if (!thumb.empty()) unlink(thumb.c_str());
    });
    std::string id = it.id;
    clip_list_.erase(std::remove_if(clip_list_.begin(), clip_list_.end(), [&](const ClipItem& x) { return x.id == id; }),
                     clip_list_.end());
    request();
}

// The full item for the right pane: all its text, or the image itself.
void App::clip_select(const ClipItem& it) {
    if (clip_sel_id_ == it.id && clip_detail_id_ == it.id) return;
    clip_sel_id_ = it.id;
    clip_text_scroll_.reset(0);
    if (clip_detail_img_) {
        cairo_surface_destroy(clip_detail_img_);
        clip_detail_img_ = nullptr;
    }
    clip_detail_text_.clear();
    clip_detail_id_ = it.id;
    clip_detail_loading_ = true;
    std::string line = it.line, thumb = it.thumb, id = it.id;
    bool image = it.kind == 1;
    auto text = std::make_shared<std::string>();
    auto img = std::make_shared<cairo_surface_t*>(nullptr);
    loop_.async(
        [line, thumb, image, text, img] {
            if (image) {
                if (!file_exists(thumb)) {
                    auto r = proc::run({"cliphist", "decode"}, 4000, &line);
                    if (r.ok()) write_file_atomic(thumb, r.out);
                }
                *img = load_image_blocking(thumb, 1200);
            } else {
                auto r = proc::run({"cliphist", "decode"}, 4000, &line);
                *text = r.ok() ? valid_utf8(r.out) : "";
                if (text->size() > 200000) *text = text->substr(0, 200000) + "\n…";
            }
        },
        [this, id, text, img] {
            if (clip_detail_id_ != id) {
                if (*img) cairo_surface_destroy(*img);
                return;
            }
            clip_detail_loading_ = false;
            clip_detail_text_ = *text;
            clip_detail_img_ = *img;
            request();
        });
}

void App::clip_paste(const ClipItem& it) {
    // Like noctalia: it lands in the input that had focus before the panel
    // (the island never took it, or gives it back by closing).
    std::string line = it.line;
    bool image = it.kind == 1;
    close_all();
    loop_.async(
        [line] {
            auto r = proc::run({"cliphist", "decode"}, 4000, &line);
            if (r.ok()) proc::run_quiet({"wl-copy"}, &r.out);
        },
        [this, image] {
            // Give focus back to the window that had it, then paste there.
            uint64_t win = clip_target_win_;
            if (win) niri_.focus_window(win);
            loop_.after(win ? 0.22 : 0.15, [this, win, image] {
                if (!proc::has("wtype")) {
                    show_toast("Copiado (instala wtype para pegar directo)");
                    return;
                }
                std::string app = lower(clip_target_app_);
                if (!win)
                    if (const auto* w = niri_.focused_window()) app = lower(w->app_id);
                static const char* terms[] = {"ghostty", "kitty", "alacritty", "foot", "wezterm", "konsole", "terminal", "xterm"};
                bool term = false;
                for (const char* t : terms)
                    if (app.find(t) != std::string::npos) term = true;
                // Terminals paste text with Ctrl+Shift+V; images go through Ctrl+V,
                // which apps inside them (Claude Code…) read from the clipboard.
                if (term && !image) proc::spawn({"wtype", "-M", "ctrl", "-M", "shift", "v", "-m", "shift", "-m", "ctrl"});
                else proc::spawn({"wtype", "-M", "ctrl", "v", "-m", "ctrl"});
            });
        });
}

void App::draw_clipboard(cairo_t* cr, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    w::header(c, ID("clip.back"), w, "Portapapeles",
              clip_panel_loading_ ? "Cargando…" : std::to_string(clip_list_.size()) + " elementos · doble clic pega")
        .click([this] { back(); });

    bool armed = now_mono() - clip_wipe_armed_ < 3.0;
    w::pill(c, ID("clip.wipe"), {w - 16 - 130, 15, 130, 30}, armed ? "¿Seguro? Borrar" : "Borrar todo", "󰆴",
            armed ? w::Style::Danger : w::Style::Plain, 12)
        .click([this, armed] {
            if (!armed) {
                clip_wipe_armed_ = now_mono();
                loop_.after(3.05, [this] { request(); });
                return;
            }
            clip_wipe_armed_ = 0;
            loop_.async([] {
                proc::run({"cliphist", "wipe"}, 4000);
                proc::run({"sh", "-c", "rm -f \"$0\"/*.img", clip_cache_dir()}, 4000);
            });
            clip_list_.clear();
            clip_sel_id_.clear();
            show_toast("Historial borrado");
        });

    // ── left: search, filters, list ──────────────────────────────────
    float lw = 300;
    Rect field{16, 58, lw, 36};
    fill(cr, field, 18, p.text.with_alpha(clip_search_focus_ ? 0.1f : 0.06f));
    if (clip_search_focus_) stroke(cr, field, 18, p.accent.with_alpha(0.6f), 1.2f);
    glyph(cr, field.x + 18, field.cy(), "󰍉", 14, p.text_dim);
    Text qt{ui(13), p.text};
    float tx = field.x + 34;
    if (clip_query_.text.empty())
        text(cr, tx, field.cy() - 9, clip_search_focus_ ? "Escribe para buscar…" : "Buscar (clic aquí)", {ui(13), p.text_faint});
    else text(cr, tx, field.cy() - 9, clip_query_.text, {ui(13), p.text, Align::Left, field.w - 46});
    if (clip_search_focus_ && std::fmod(anim_time_, 1.1f) < 0.7f)
        fill(cr, {tx + caret_x(clip_query_.text, qt, clip_query_.caret), field.cy() - 9, 2, 18}, 1, p.accent);
    ui_.add(ID("clip.search"), field).cursor_shape(CursorShape::Text).click([this] {
        clip_search_focus_ = true;
        update_keyboard();
    });

    const char* glyphs[4] = {"󰅍", "󰊄", "󰋩", "󰌹"};
    const char* tips[4] = {"Todo", "Texto", "Imágenes", "Enlaces"};
    for (int i = 0; i < 4; ++i) {
        Rect r{16 + i * (lw + 6) / 4.0f, 102, (lw - 18) / 4.0f, 30};
        w::pill(c, ID("clip.f", i), r, tips[i], glyphs[i], clip_filter_ == i ? w::Style::Accent : w::Style::Plain, 11)
            .click([this, i] {
                clip_filter_ = i;
                clip_list_scroll_.reset(0);
                clip_sel_id_.clear();
            });
    }

    std::string q = lower(clip_query_.text);
    std::vector<const ClipItem*> shown;
    for (auto& it : clip_list_) {
        if (clip_filter_ == 1 && (it.kind == 1 || it.kind == 2)) continue;
        if (clip_filter_ == 2 && it.kind != 1) continue;
        if (clip_filter_ == 3 && it.kind != 2) continue;
        if (!q.empty() && lower(it.content).find(q) == std::string::npos) continue;
        shown.push_back(&it);
    }
    int sel = -1;
    for (size_t i = 0; i < shown.size(); ++i)
        if (shown[i]->id == clip_sel_id_) sel = static_cast<int>(i);
    if (sel < 0 && !shown.empty()) {
        sel = 0;
        clip_select(*shown[0]);
    }
    Rect area{16, 142, lw, h - 142 - 14};
    float rowh = 52;
    // Keyboard (only while searching): arrows move, Enter pastes.
    if (clip_key_nav_ != 0 && !shown.empty()) {
        if (clip_key_nav_ == 2) {
            ClipItem it = *shown[sel];
            clip_key_nav_ = 0;
            clip_paste(it);
            return;
        }
        sel = std::clamp(sel + clip_key_nav_, 0, static_cast<int>(shown.size()) - 1);
        clip_select(*shown[sel]);
        float top = sel * rowh;
        if (top < clip_list_scroll_.target) clip_list_scroll_.set(top);
        if (top + rowh > clip_list_scroll_.target + area.h) clip_list_scroll_.set(top + rowh - area.h);
    }
    clip_key_nav_ = 0;

    float content = shown.size() * rowh;
    ui_.add(ID("clip.area"), area).scroll([this, content, area](float dy) {
        clip_list_scroll_.set(std::clamp(clip_list_scroll_.target + dy * 60, 0.0f, std::max(0.0f, content - area.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, area.x - 2, area.y, area.w + 4, area.h);
    cairo_clip(cr);
    ui_.push_clip(area);
    if (shown.empty()) {
        const char* msg = !proc::has("cliphist") ? "cliphist no está instalado"
                        : clip_panel_loading_    ? "Cargando historial…"
                        : q.empty()              ? "Nada por aquí todavía"
                                                 : "Sin coincidencias";
        text(cr, area.cx(), area.y + 30, msg, {ui(12.5f), p.text_dim, Align::Center});
    }
    for (size_t i = 0; i < shown.size(); ++i) {
        const ClipItem& it = *shown[i];
        Rect r{area.x, area.y + i * rowh - clip_list_scroll_.value, area.w, rowh - 4};
        if (r.bottom() < area.y || r.y > area.bottom()) continue;
        uint64_t id = ID("clip.row", static_cast<int>(std::hash<std::string>{}(it.id) & 0x7fffffff));
        float hv = ui_.hover(id);
        bool on = static_cast<int>(i) == sel;
        fill(cr, r, 12, on ? p.accent_dim : p.text.with_alpha(0.03f + 0.05f * hv));
        Rect ic{r.x + 8, r.y + 6, 36, 36};
        if (it.kind == 1) {
            if (cairo_surface_t* img = ImageCache::get().request(it.thumb, 96)) image(cr, img, ic, 8);
            else {
                fill(cr, ic, 8, p.text.with_alpha(0.08f));
                glyph(cr, ic.cx(), ic.cy(), "󰋩", 15, p.text_dim);
            }
        } else if (it.kind == 3) {
            fill(cr, ic, 8, it.color);
        } else {
            fill(cr, ic, 8, p.text.with_alpha(0.08f));
            glyph(cr, ic.cx(), ic.cy(), it.kind == 2 ? "󰌹" : "󰦨", 15, on ? p.accent : p.text_dim);
        }
        std::string title = it.kind == 1 ? "Imagen" : trim(replace_all(it.content, "\n", " "));
        std::string sub = it.kind == 1 ? it.meta : it.kind == 2 ? it.meta : it.kind == 3 ? "Color" : "Texto";
        text(cr, r.x + 54, r.y + 8, title, {ui(12.5f, "SemiBold"), p.text, Align::Left, r.w - 64});
        text(cr, r.x + 54, r.y + 26, sub, {ui(10.5f), on ? p.accent : p.text_faint, Align::Left, r.w - 64});
        ClipItem copy = it;
        ui_.add(id, r).click([this, copy] {
            // Second click on the same row within half a second: paste it.
            double t = now_mono();
            bool dbl = clip_last_click_id_ == copy.id && t - clip_last_click_ < 0.45;
            clip_last_click_ = t;
            clip_last_click_id_ = copy.id;
            if (dbl) {
                clip_paste(copy);
                return;
            }
            clip_select(copy);
        });
    }
    ui_.pop_clip();
    cairo_restore(cr);
    w::scrollbar(c, {area.right() + 4, area.y + 2, 3, area.h - 4}, clip_list_scroll_.value, content, area.h);

    // ── right: the selected item, whole ──────────────────────────────
    Rect dr{lw + 32, 58, w - lw - 48, h - 58 - 14};
    fill(cr, dr, 16, p.text.with_alpha(0.04f));
    if (sel < 0) return;
    const ClipItem& it = *shown[sel];
    ClipItem copy = it;
    // Actions
    float bx = dr.x + 12, by = dr.y + 10;
    auto act = [&](const char* key, const char* label, const char* g, w::Style st, std::function<void()> fn) {
        float bw = measure(label, {ui(12, "SemiBold")}).w + 40;
        w::pill(c, ID(key), {bx, by, bw, 30}, label, g, st, 12).click(std::move(fn));
        bx += bw + 6;
    };
    act("clip.paste", "Pegar", "󰆒", w::Style::Accent, [this, copy] { clip_paste(copy); });
    act("clip.copy", "Copiar", "󰆏", w::Style::Tonal, [this, copy] { clip_copy(copy); });
    if (it.kind == 2)
        act("clip.open", "Abrir", "󰏌", w::Style::Plain, [this, copy] {
            proc::spawn({"xdg-open", trim(copy.content)});
            close_all();
        });
    if (it.kind == 1) {
        act("clip.edit", "Editar", "󰏫", w::Style::Plain, [this, copy] {
            std::string path = copy.thumb;
            auto img = std::make_shared<cairo_surface_t*>(nullptr);
            close_all();
            loop_.async([path, img] { *img = load_image_full(path); },
                        [this, img] {
                            if (*img) capture_.edit(*img, true, false);
                            else show_toast("No se pudo abrir la imagen");
                        });
        });
        act("clip.save", "Guardar", "󰆓", w::Style::Plain, [this, copy] {
            std::string dir = user_dir("PICTURES", "Imágenes") + "/Portapapeles";
            std::string ext = "png";
            for (const char* e : {"jpeg", "jpg", "webp", "gif", "bmp"})
                if (copy.meta.find(e) != std::string::npos) ext = e;
            std::string src = copy.thumb, dst = dir + "/" + fmt_time("Portapapeles_%Y-%m-%d_%H-%M-%S.") + ext;
            loop_.async([dir, src, dst] {
                make_dirs(dir);
                proc::run({"cp", src, dst}, 4000);
            });
            show_toast("Guardada en Imágenes/Portapapeles");
        });
    }
    w::icon_button(c, ID("clip.del"), {dr.right() - 42, by - 1, 32, 32}, "󰆴", 14, false, p.bad).click([this, copy] {
        clip_delete(copy);
        clip_sel_id_.clear();
    });

    Rect body{dr.x + 14, dr.y + 52, dr.w - 28, dr.h - 64};
    if (clip_detail_loading_ && clip_detail_id_ == it.id) {
        text(cr, body.cx(), body.cy() - 8, "Cargando…", {ui(12.5f), p.text_dim, Align::Center});
        return;
    }
    if (it.kind == 1) {
        // The image whole (contained, not cropped), at full opacity.
        if (cairo_surface_t* img = clip_detail_img_) {
            int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
            double s = std::min(static_cast<double>(body.w) / iw, static_cast<double>(body.h) / ih);
            float dw = iw * s, dh = ih * s;
            Rect ir{body.cx() - dw / 2, body.cy() - dh / 2, dw, dh};
            cairo_save(cr);
            rrect(cr, ir, 10);
            cairo_clip(cr);
            cairo_translate(cr, ir.x, ir.y);
            cairo_scale(cr, s, s);
            cairo_set_source_surface(cr, img, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
            cairo_restore(cr);
            text(cr, body.x, body.bottom() - 4, it.meta, {ui(10.5f), p.text_faint});
        }
        return;
    }
    if (it.kind == 3) fill(cr, {body.x, body.y, body.w, 70}, 12, it.color);
    // Text: all of it, wrapped, with its own scroll.
    float off = it.kind == 3 ? 82 : 0;
    Rect tb{body.x, body.y + off, body.w - 8, body.h - off};
    Text tt{it.kind == 2 ? mono(12.5f) : ui(13), p.text, Align::Left, tb.w, 0, false, true};
    Size ts = measure(clip_detail_text_, tt);
    float th = ts.h + 8;
    ui_.add(ID("clip.text"), tb).scroll([this, th, tb](float dy) {
        clip_text_scroll_.set(std::clamp(clip_text_scroll_.target + dy * 60, 0.0f, std::max(0.0f, th - tb.h)));
    });
    cairo_save(cr);
    cairo_rectangle(cr, tb.x, tb.y, tb.w + 8, tb.h);
    cairo_clip(cr);
    text(cr, tb.x, tb.y - clip_text_scroll_.value, clip_detail_text_, tt);
    cairo_restore(cr);
    w::scrollbar(c, {tb.right() + 4, tb.y + 2, 3, tb.h - 4}, clip_text_scroll_.value, th, tb.h);
}

}  // namespace island
