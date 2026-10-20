#include "suite/capture.hpp"

#include <memory>
#include <pango/pangocairo.h>

#include "core/config.hpp"
#include "core/loop.hpp"
#include "core/proc.hpp"
#include "ui/paint.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace island {

using namespace paint;

static const Color kPalette[] = {Color::hex(0xFF453A), Color::hex(0xFF9F0A), Color::hex(0xFFD60A), Color::hex(0x32D74B),
                                 Color::hex(0x0A84FF), Color::hex(0xBF5AF2), Color::hex(0xFFFFFF), Color::hex(0x111111)};
static constexpr int kColors = 8;

Rect Shape::bounds() const {
    if (pts.size() < 2) return {};
    float x0 = pts[0], y0 = pts[1], x1 = x0, y1 = y0;
    for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        x0 = std::min(x0, pts[i]);
        y0 = std::min(y0, pts[i + 1]);
        x1 = std::max(x1, pts[i]);
        y1 = std::max(y1, pts[i + 1]);
    }
    if (kind == Text) {
        Size s = measure(text.empty() ? " " : text, {ui(width * 5 + 12, "Bold")});
        return {x0, y0, std::max(s.w, 4.0f), std::max(s.h, 4.0f)};
    }
    if (kind == Counter) {
        float r = width * 2.2f + 11;
        return {x0 - r, y0 - r, 2 * r, 2 * r};
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

Capture::~Capture() {
    if (img_) cairo_surface_destroy(img_);
}

void Capture::reset() {
    shapes_.clear();
    undo_.clear();
    redo_.clear();
    selected_ = -1;
    typing_ = false;
    text_.clear();
    drag_ = Drag::None;
    has_sel_ = false;
    busy_ = false;
}

void Capture::edit(cairo_surface_t* img, bool select_all, bool screen) {
    if (img_) cairo_surface_destroy(img_);
    img_ = img;
    iw_ = cairo_image_surface_get_width(img);
    ih_ = cairo_image_surface_get_height(img);
    screen_ = screen;
    picking_ = false;
    reset();
    if (select_all) {
        sel_ = {0, 0, static_cast<float>(iw_), static_cast<float>(ih_)};
        has_sel_ = true;
    }
    open();
}

void Capture::select_region(std::function<void(const Rect&)> done, std::function<void()> cancelled) {
    if (img_) {
        cairo_surface_destroy(img_);
        img_ = nullptr;
    }
    picking_ = true;
    screen_ = true;
    reset();
    region_done_ = std::move(done);
    region_cancel_ = std::move(cancelled);
    open();
}

void Capture::closed() {
    if (img_) {
        cairo_surface_destroy(img_);
        img_ = nullptr;
    }
    region_done_ = nullptr;
    region_cancel_ = nullptr;
}

// ── geometry helpers ───────────────────────────────────────────────────────

int Capture::handle_at(float x, float y) const {
    if (!has_sel_) return -1;
    Rect s = sel_screen();
    float hx[3] = {s.x, s.cx(), s.right()}, hy[3] = {s.y, s.cy(), s.bottom()};
    int idx = 0;
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (i == 1 && j == 1) continue;
            if (std::fabs(x - hx[i]) <= 9 && std::fabs(y - hy[j]) <= 9) return idx;
            ++idx;
        }
    return -1;
}

int Capture::shape_at(float ix, float iy) const {
    for (int i = static_cast<int>(shapes_.size()) - 1; i >= 0; --i) {
        Rect b = shapes_[i].bounds().inset(-(shapes_[i].width + 6));
        if (b.w < 12) b = b.inset((b.w - 12) / 2, 0);
        if (b.h < 12) b = b.inset(0, (b.h - 12) / 2);
        if (b.contains(ix, iy)) return i;
    }
    return -1;
}

void Capture::push_undo() {
    undo_.push_back(shapes_);
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
}

void Capture::undo() {
    if (undo_.empty()) return;
    redo_.push_back(shapes_);
    shapes_ = undo_.back();
    undo_.pop_back();
    selected_ = -1;
}

void Capture::redo() {
    if (redo_.empty()) return;
    undo_.push_back(shapes_);
    shapes_ = redo_.back();
    redo_.pop_back();
    selected_ = -1;
}

void Capture::commit_text() {
    if (!typing_) return;
    typing_ = false;
    if (trim(text_.text).empty()) return;
    push_undo();
    Shape s;
    s.kind = Shape::Text;
    s.pts = {text_x_, text_y_};
    s.color = kPalette[color_];
    s.width = width_;
    s.text = text_.text;
    shapes_.push_back(s);
    text_.clear();
}

// ── drawing shapes (image coordinates) ─────────────────────────────────────

static void arrow_head(cairo_t* cr, float x0, float y0, float x1, float y1, float w) {
    float ang = std::atan2(y1 - y0, x1 - x0);
    float len = w * 3.4f + 10, spread = 0.46f;
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x1 - len * std::cos(ang - spread), y1 - len * std::sin(ang - spread));
    cairo_line_to(cr, x1 - len * 0.72f * std::cos(ang), y1 - len * 0.72f * std::sin(ang));
    cairo_line_to(cr, x1 - len * std::cos(ang + spread), y1 - len * std::sin(ang + spread));
    cairo_close_path(cr);
    cairo_fill(cr);
}

void Capture::draw_shape(cairo_t* cr, const Shape& s, bool preview) const {
    (void)preview;
    if (s.pts.size() < 2) return;
    cairo_save(cr);
    set(cr, s.color);
    cairo_set_line_width(cr, s.width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    float x0 = s.pts[0], y0 = s.pts[1];
    float x1 = s.pts.size() >= 4 ? s.pts[s.pts.size() - 2] : x0, y1 = s.pts.size() >= 4 ? s.pts.back() : y0;
    Rect r{std::min(x0, x1), std::min(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0)};
    switch (s.kind) {
        case Shape::Rect:
            rrect(cr, r, std::min(6.0f, s.width * 1.5f));
            if (s.filled) {
                set(cr, s.color.with_alpha(0.35f));
                cairo_fill_preserve(cr);
                set(cr, s.color);
            }
            cairo_stroke(cr);
            break;
        case Shape::Ellipse:
            if (r.w > 1 && r.h > 1) {
                cairo_save(cr);
                cairo_translate(cr, r.cx(), r.cy());
                cairo_scale(cr, r.w / 2, r.h / 2);
                cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
                cairo_restore(cr);
                if (s.filled) {
                    set(cr, s.color.with_alpha(0.35f));
                    cairo_fill_preserve(cr);
                    set(cr, s.color);
                }
                cairo_stroke(cr);
            }
            break;
        case Shape::Line:
            cairo_move_to(cr, x0, y0);
            cairo_line_to(cr, x1, y1);
            cairo_stroke(cr);
            break;
        case Shape::Arrow: {
            float ang = std::atan2(y1 - y0, x1 - x0);
            float back = (s.width * 3.4f + 10) * 0.7f;
            float d = std::hypot(x1 - x0, y1 - y0);
            if (d > back) {
                cairo_move_to(cr, x0, y0);
                cairo_line_to(cr, x1 - back * std::cos(ang), y1 - back * std::sin(ang));
                cairo_stroke(cr);
            }
            arrow_head(cr, x0, y0, x1, y1, s.width);
            break;
        }
        case Shape::Pen:
        case Shape::Marker: {
            if (s.kind == Shape::Marker) {
                set(cr, s.color.with_alpha(0.38f));
                cairo_set_line_width(cr, s.width * 4.5f);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
            }
            cairo_move_to(cr, x0, y0);
            size_t n = s.pts.size() / 2;
            if (n == 1) cairo_line_to(cr, x0 + 0.01, y0);
            for (size_t i = 1; i < n; ++i) {
                float px = s.pts[2 * (i - 1)], py = s.pts[2 * (i - 1) + 1];
                float cx = s.pts[2 * i], cy = s.pts[2 * i + 1];
                cairo_curve_to(cr, px, py, (px + cx) / 2, (py + cy) / 2, (px + cx) / 2, (py + cy) / 2);
            }
            if (n > 1) cairo_line_to(cr, x1, y1);
            cairo_stroke(cr);
            break;
        }
        case Shape::Text: {
            PangoLayout* l = pango_cairo_create_layout(cr);
            PangoFontDescription* fd = pango_font_description_from_string(ui(s.width * 5 + 12, "Bold").c_str());
            pango_layout_set_font_description(l, fd);
            pango_layout_set_text(l, s.text.c_str(), -1);
            cairo_move_to(cr, x0, y0);
            pango_cairo_layout_path(cr, l);
            // A dark rim keeps any colour readable on any background.
            bool light = s.color.luminance() > 0.5f;
            cairo_set_source_rgba(cr, light ? 0 : 1, light ? 0 : 1, light ? 0 : 1, 0.55);
            cairo_set_line_width(cr, std::max(2.0f, s.width * 0.9f));
            cairo_stroke_preserve(cr);
            set(cr, s.color);
            cairo_fill(cr);
            pango_font_description_free(fd);
            g_object_unref(l);
            break;
        }
        case Shape::Blur: {
            if (!img_ || r.w < 2 || r.h < 2) break;
            // Pixelate: shrink the region, then paint it back with nearest-neighbour.
            float block = std::max(6.0f, s.width * 3.0f);
            int sw = std::max(1, static_cast<int>(r.w / block)), sh = std::max(1, static_cast<int>(r.h / block));
            cairo_surface_t* small = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
            cairo_t* sc = cairo_create(small);
            cairo_scale(sc, sw / r.w, sh / r.h);
            cairo_set_source_surface(sc, img_, -r.x, -r.y);
            cairo_pattern_set_filter(cairo_get_source(sc), CAIRO_FILTER_GOOD);
            cairo_paint(sc);
            cairo_destroy(sc);
            cairo_rectangle(cr, r.x, r.y, r.w, r.h);
            cairo_clip(cr);
            cairo_translate(cr, r.x, r.y);
            cairo_scale(cr, r.w / sw, r.h / sh);
            cairo_set_source_surface(cr, small, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
            cairo_paint(cr);
            cairo_surface_destroy(small);
            break;
        }
        case Shape::Counter: {
            float rad = s.width * 2.2f + 11;
            cairo_arc(cr, x0, y0, rad, 0, 2 * M_PI);
            set(cr, s.color);
            cairo_fill_preserve(cr);
            cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
            cairo_set_line_width(cr, 2);
            cairo_stroke(cr);
            Color tc = s.color.luminance() > 0.6f ? Color{0.08f, 0.08f, 0.1f, 1} : Color{1, 1, 1, 1};
            Text t{ui(rad * 1.05f, "Bold"), tc, Align::Center};
            Size ts = measure(std::to_string(s.number), t);
            text(cr, x0, y0 - ts.h / 2, std::to_string(s.number), t);
            break;
        }
    }
    cairo_restore(cr);
}

cairo_surface_t* Capture::render_result() {
    if (!img_ || !has_sel_) return nullptr;
    int w = std::max(1, static_cast<int>(std::lround(sel_.w))), h = std::max(1, static_cast<int>(std::lround(sel_.h)));
    cairo_surface_t* out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t* cr = cairo_create(out);
    cairo_translate(cr, -std::round(sel_.x), -std::round(sel_.y));
    cairo_set_source_surface(cr, img_, 0, 0);
    cairo_paint(cr);
    for (auto& s : shapes_) draw_shape(cr, s, false);
    cairo_destroy(cr);
    return out;
}

static cairo_status_t png_to_string(void* closure, const unsigned char* data, unsigned int len) {
    static_cast<std::string*>(closure)->append(reinterpret_cast<const char*>(data), len);
    return CAIRO_STATUS_SUCCESS;
}

void Capture::save_async(cairo_surface_t* out, bool save, bool copy,
                         std::function<void(const std::string&, cairo_surface_t*)> done) {
    // A small copy for the notification.
    int ow = cairo_image_surface_get_width(out), oh = cairo_image_surface_get_height(out);
    float ts = std::min(1.0f, 160.0f / std::max(ow, oh));
    cairo_surface_t* thumb =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, std::max(1, int(ow * ts)), std::max(1, int(oh * ts)));
    cairo_t* tc = cairo_create(thumb);
    cairo_scale(tc, ts, ts);
    cairo_set_source_surface(tc, out, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(tc), CAIRO_FILTER_GOOD);
    cairo_paint(tc);
    cairo_destroy(tc);

    std::string dir = config().screenshot_dir.empty() ? user_dir("PICTURES", "Imágenes") + "/Capturas"
                                                      : expand_home(config().screenshot_dir);
    std::string path = save ? dir + "/" + fmt_time("Captura_%Y-%m-%d_%H-%M-%S.png") : "";
    auto ok = std::make_shared<bool>(true);
    Loop::get().async(
        [out, path, copy, dir, ok] {
            std::string png;
            cairo_surface_write_to_png_stream(out, png_to_string, &png);
            cairo_surface_destroy(out);
            if (!path.empty()) {
                make_dirs(dir);
                *ok = write_file_atomic(path, png);
            }
            if (copy) proc::run_quiet({"wl-copy", "--type", "image/png"}, &png);
        },
        [path, thumb, ok, done] {
            if (done) done(*ok ? path : "", thumb);
            else cairo_surface_destroy(thumb);
        });
}

void Capture::finish(bool save, bool copy) {
    if (busy_) return;
    commit_text();
    cairo_surface_t* out = render_result();
    if (!out) return;
    busy_ = true;
    save_async(out, save, copy, [this, copy](const std::string& path, cairo_surface_t* thumb) {
        busy_ = false;
        if (on_saved) on_saved(path, thumb, copy);
        else cairo_surface_destroy(thumb);
    });
    close();
}

// ── input ──────────────────────────────────────────────────────────────────

void Capture::canvas_button(Button b, bool pressed, float x, float y) {
    if (b == Button::Right && pressed) {
        if (drag_ != Drag::None) {
            // Right click while drawing drops the shape being drawn.
            bool drawing = drag_ == Drag::Draw;
            drag_ = Drag::None;
            if (drawing && !shapes_.empty()) {
                shapes_.pop_back();
                if (!undo_.empty()) undo_.pop_back();
            }
            return;
        }
        color_ = (color_ + 1) % kColors;
        if (on_toast) on_toast("Color cambiado");
        return;
    }
    if (b != Button::Left) return;
    float ix = to_ix(x), iy = to_iy(y);
    if (pressed) {
        if (typing_) commit_text();
        int h = handle_at(x, y);
        bool inside = has_sel_ && sel_screen().contains(x, y);
        if (!has_sel_ || picking_ || tool_ == Tool::Select) {
            if (h >= 0) {
                drag_ = Drag::Resize;
                handle_ = h;
                start_sel_ = sel_;
                ax_ = ix;
                ay_ = iy;
                return;
            }
            if (!picking_ && has_sel_ && tool_ == Tool::Select) {
                int si = shape_at(ix, iy);
                if (si >= 0) {
                    push_undo();
                    selected_ = si;
                    drag_ = Drag::ShapeMove;
                    start_pts_ = shapes_[si].pts;
                    ax_ = ix;
                    ay_ = iy;
                    return;
                }
            }
            if (inside) {
                drag_ = Drag::Move;
                start_sel_ = sel_;
                ax_ = ix;
                ay_ = iy;
                selected_ = -1;
                return;
            }
            // New selection.
            drag_ = Drag::Create;
            ax_ = std::clamp(ix, 0.0f, static_cast<float>(screen_ ? width() / vs_ : iw_));
            ay_ = std::clamp(iy, 0.0f, static_cast<float>(screen_ ? height() / vs_ : ih_));
            sel_ = {ax_, ay_, 0, 0};
            has_sel_ = true;
            selected_ = -1;
            return;
        }
        // Drawing tools.
        selected_ = -1;
        if (tool_ == Tool::Text) {
            typing_ = true;
            text_.clear();
            text_x_ = ix;
            text_y_ = iy - (width_ * 5 + 12) * 0.6f;
            return;
        }
        push_undo();
        Shape s;
        s.color = kPalette[color_];
        s.width = width_;
        s.filled = fill_;
        s.pts = {ix, iy, ix, iy};
        switch (tool_) {
            case Tool::Rect: s.kind = Shape::Rect; break;
            case Tool::Ellipse: s.kind = Shape::Ellipse; break;
            case Tool::Arrow: s.kind = Shape::Arrow; break;
            case Tool::Line: s.kind = Shape::Line; break;
            case Tool::Pen: s.kind = Shape::Pen; break;
            case Tool::Marker: s.kind = Shape::Marker; break;
            case Tool::Blur: s.kind = Shape::Blur; break;
            case Tool::Counter: {
                s.kind = Shape::Counter;
                int n = 0;
                for (auto& o : shapes_)
                    if (o.kind == Shape::Counter) n = std::max(n, o.number);
                s.number = n + 1;
                s.pts = {ix, iy};
                shapes_.push_back(s);
                return;
            }
            default: break;
        }
        shapes_.push_back(s);
        drag_ = Drag::Draw;
        return;
    }
    // release
    Drag was = drag_;
    drag_ = Drag::None;
    if (was == Drag::Create) {
        if (sel_.w < 4 || sel_.h < 4) {
            // A click instead of a drag selects everything.
            sel_ = screen_ ? Rect{0, 0, width() / vs_, height() / vs_}
                           : Rect{0, 0, static_cast<float>(iw_), static_cast<float>(ih_)};
            if (img_) sel_ = sel_.intersect({0, 0, static_cast<float>(iw_), static_cast<float>(ih_)});
        }
    } else if (was == Drag::Draw && !shapes_.empty()) {
        Shape& s = shapes_.back();
        Rect bb = s.bounds();
        bool tiny = s.kind != Shape::Pen && s.kind != Shape::Marker && bb.w < 3 && bb.h < 3;
        if (tiny) {
            shapes_.pop_back();
            if (!undo_.empty()) undo_.pop_back();
        }
    }
}

void Capture::canvas_motion(float x, float y) {
    float ix = to_ix(x), iy = to_iy(y);
    float maxw = img_ ? iw_ : width() / vs_, maxh = img_ ? ih_ : height() / vs_;
    ix = std::clamp(ix, 0.0f, maxw);
    iy = std::clamp(iy, 0.0f, maxh);
    switch (drag_) {
        case Drag::Create:
            sel_ = {std::min(ax_, ix), std::min(ay_, iy), std::fabs(ix - ax_), std::fabs(iy - ay_)};
            break;
        case Drag::Move: {
            float nx = std::clamp(start_sel_.x + ix - ax_, 0.0f, maxw - start_sel_.w);
            float ny = std::clamp(start_sel_.y + iy - ay_, 0.0f, maxh - start_sel_.h);
            sel_ = {nx, ny, start_sel_.w, start_sel_.h};
            break;
        }
        case Drag::Resize: {
            float l = start_sel_.x, t = start_sel_.y, r = start_sel_.right(), b = start_sel_.bottom();
            // handles: 0 tl 1 t 2 tr 3 l 4 r 5 bl 6 b 7 br
            int hcol = handle_ == 0 || handle_ == 3 || handle_ == 5 ? 0 : handle_ == 1 || handle_ == 6 ? 1 : 2;
            int hrow = handle_ <= 2 ? 0 : handle_ <= 4 ? 1 : 2;
            if (hcol == 0) l = ix;
            if (hcol == 2) r = ix;
            if (hrow == 0) t = iy;
            if (hrow == 2) b = iy;
            sel_ = {std::min(l, r), std::min(t, b), std::fabs(r - l), std::fabs(b - t)};
            break;
        }
        case Drag::Draw: {
            if (shapes_.empty()) break;
            Shape& s = shapes_.back();
            if (s.kind == Shape::Pen || s.kind == Shape::Marker) {
                float lx = s.pts[s.pts.size() - 2], ly = s.pts.back();
                if (std::hypot(ix - lx, iy - ly) * vs_ > 2.0f) {
                    if (s.pts.size() == 4 && s.pts[0] == s.pts[2] && s.pts[1] == s.pts[3]) s.pts.resize(2);
                    s.pts.push_back(ix);
                    s.pts.push_back(iy);
                }
            } else {
                s.pts[2] = ix;
                s.pts[3] = iy;
            }
            break;
        }
        case Drag::ShapeMove:
            if (selected_ >= 0 && selected_ < static_cast<int>(shapes_.size())) {
                auto& pts = shapes_[selected_].pts;
                for (size_t i = 0; i + 1 < pts.size() && i + 1 < start_pts_.size(); i += 2) {
                    pts[i] = start_pts_[i] + ix - ax_;
                    pts[i + 1] = start_pts_[i + 1] + iy - ay_;
                }
            }
            break;
        default: break;
    }
}

void Capture::canvas_scroll(float dy) {
    if (picking_) return;
    width_ = std::clamp(width_ - dy, 1.0f, 30.0f);
    if (selected_ >= 0 && selected_ < static_cast<int>(shapes_.size())) shapes_[selected_].width = width_;
}

CursorShape Capture::canvas_cursor() const {
    if (drag_ == Drag::Move || drag_ == Drag::ShapeMove) return CursorShape::Grabbing;
    float x = px_, y = py_;
    int h = handle_at(x, y);
    if (h >= 0 && (picking_ || tool_ == Tool::Select || !has_sel_)) {
        static const CursorShape map[] = {CursorShape::ResizeNW, CursorShape::ResizeN, CursorShape::ResizeNE,
                                          CursorShape::ResizeW,  CursorShape::ResizeE, CursorShape::ResizeSW,
                                          CursorShape::ResizeS,  CursorShape::ResizeSE};
        return map[h];
    }
    if (!has_sel_ || drag_ == Drag::Create) return CursorShape::Crosshair;
    if (picking_ || tool_ == Tool::Select) return sel_screen().contains(x, y) ? CursorShape::Move : CursorShape::Crosshair;
    if (tool_ == Tool::Text) return CursorShape::Text;
    return CursorShape::Crosshair;
}

void Capture::key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    bool ctrl = mods & Wayland::MOD_CTRL, shift = mods & Wayland::MOD_SHIFT;
    if (typing_) {
        if (sym == XKB_KEY_Escape) {
            typing_ = false;
            text_.clear();
            return;
        }
        if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && !shift) {
            commit_text();
            return;
        }
        if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && shift) {
            text_.insert("\n");
            return;
        }
        text_.multiline = true;
        text_.key(sym, utf8, mods);
        return;
    }
    if (picking_) {
        if (sym == XKB_KEY_Escape) {
            auto cb = region_cancel_;
            close();
            if (cb) cb();
        } else if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && has_sel_ && sel_.w > 8 && sel_.h > 8) {
            auto cb = region_done_;
            Rect r = sel_;
            close();
            if (cb) cb(r);
        }
        return;
    }
    if (sym == XKB_KEY_Escape) {
        if (selected_ >= 0) selected_ = -1;
        else close();
        return;
    }
    if (ctrl && (sym == XKB_KEY_z || sym == XKB_KEY_Z)) {
        shift ? redo() : undo();
        return;
    }
    if (ctrl && (sym == XKB_KEY_y || sym == XKB_KEY_Y)) {
        redo();
        return;
    }
    if (ctrl && (sym == XKB_KEY_c || sym == XKB_KEY_C)) {
        finish(false, true);
        return;
    }
    if (ctrl && (sym == XKB_KEY_s || sym == XKB_KEY_S)) {
        finish(true, false);
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        if (has_sel_) finish(true, config().screenshot_copy);
        return;
    }
    if ((sym == XKB_KEY_Delete || sym == XKB_KEY_BackSpace) && selected_ >= 0 && selected_ < static_cast<int>(shapes_.size())) {
        push_undo();
        shapes_.erase(shapes_.begin() + selected_);
        selected_ = -1;
        return;
    }
    if (ctrl) return;
    switch (sym) {
        case XKB_KEY_v: tool_ = Tool::Select; break;
        case XKB_KEY_r: tool_ = Tool::Rect; break;
        case XKB_KEY_e: tool_ = Tool::Ellipse; break;
        case XKB_KEY_a: tool_ = Tool::Arrow; break;
        case XKB_KEY_l: tool_ = Tool::Line; break;
        case XKB_KEY_p: tool_ = Tool::Pen; break;
        case XKB_KEY_m: tool_ = Tool::Marker; break;
        case XKB_KEY_t: tool_ = Tool::Text; break;
        case XKB_KEY_b: tool_ = Tool::Blur; break;
        case XKB_KEY_n: tool_ = Tool::Counter; break;
        case XKB_KEY_f: fill_ = !fill_; break;
        default:
            if (sym >= XKB_KEY_1 && sym <= XKB_KEY_8) color_ = static_cast<int>(sym - XKB_KEY_1);
            break;
    }
}

// ── drawing ────────────────────────────────────────────────────────────────

void Capture::tool_icon(cairo_t* cr, Tool t, float cx, float cy, Color c) {
    cairo_save(cr);
    set(cr, c);
    cairo_set_line_width(cr, 1.8);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    float s = 7;
    switch (t) {
        case Tool::Select:
            cairo_move_to(cr, cx - 4, cy - 7);
            cairo_line_to(cr, cx - 4, cy + 6);
            cairo_line_to(cr, cx - 0.5, cy + 2.5);
            cairo_line_to(cr, cx + 2.5, cy + 8);
            cairo_line_to(cr, cx + 4.5, cy + 7);
            cairo_line_to(cr, cx + 1.5, cy + 1.5);
            cairo_line_to(cr, cx + 6, cy + 1);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case Tool::Rect:
            rrect(cr, {cx - s, cy - s + 1.5f, 2 * s, 2 * s - 3}, 2.5f);
            cairo_stroke(cr);
            break;
        case Tool::Ellipse:
            cairo_save(cr);
            cairo_translate(cr, cx, cy);
            cairo_scale(cr, s, s - 1.5);
            cairo_arc(cr, 0, 0, 1, 0, 2 * M_PI);
            cairo_restore(cr);
            cairo_stroke(cr);
            break;
        case Tool::Arrow:
            cairo_move_to(cr, cx - 6, cy + 6);
            cairo_line_to(cr, cx + 3, cy - 3);
            cairo_stroke(cr);
            cairo_move_to(cr, cx + 7, cy - 7);
            cairo_line_to(cr, cx - 0.5, cy - 5);
            cairo_line_to(cr, cx + 5, cy + 0.5);
            cairo_close_path(cr);
            cairo_fill(cr);
            break;
        case Tool::Line:
            cairo_move_to(cr, cx - 7, cy + 7);
            cairo_line_to(cr, cx + 7, cy - 7);
            cairo_stroke(cr);
            break;
        case Tool::Pen:
            cairo_move_to(cr, cx - 7, cy + 3);
            cairo_curve_to(cr, cx - 3, cy - 7, cx, cy + 8, cx + 7, cy - 4);
            cairo_stroke(cr);
            break;
        case Tool::Marker:
            set(cr, c.with_alpha(0.55f));
            cairo_set_line_width(cr, 6);
            cairo_set_line_cap(cr, CAIRO_LINE_CAP_SQUARE);
            cairo_move_to(cr, cx - 6, cy + 2);
            cairo_line_to(cr, cx + 6, cy - 2);
            cairo_stroke(cr);
            break;
        case Tool::Text:
            text(cr, cx, cy - 10, "T", {ui(17, "Bold"), c, Align::Center});
            break;
        case Tool::Blur:
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) {
                    set(cr, c.with_alpha((i + j) % 2 ? 0.45f : 1.0f));
                    cairo_rectangle(cr, cx - 7 + i * 5, cy - 7 + j * 5, 4, 4);
                    cairo_fill(cr);
                }
            break;
        case Tool::Counter:
            cairo_arc(cr, cx, cy, 7.5, 0, 2 * M_PI);
            cairo_stroke(cr);
            text(cr, cx, cy - 7, "1", {ui(10.5f, "Bold"), c, Align::Center});
            break;
    }
    cairo_restore(cr);
}

void Capture::draw_toolbar(cairo_t* cr, const Rect& anchor, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    struct ToolDef {
        Tool t;
        const char* tip;
    };
    static const ToolDef tools[] = {{Tool::Select, "Seleccionar (V)"}, {Tool::Rect, "Rectángulo (R)"},
                                    {Tool::Ellipse, "Elipse (E)"},     {Tool::Arrow, "Flecha (A)"},
                                    {Tool::Line, "Línea (L)"},         {Tool::Pen, "Lápiz (P)"},
                                    {Tool::Marker, "Marcador (M)"},    {Tool::Text, "Texto (T)"},
                                    {Tool::Blur, "Pixelar (B)"},       {Tool::Counter, "Contador (N)"}};
    float bw = 10 + 10 * 36 + 14 + kColors * 22 + 14 + 2 * 36 + 14 + 2 * 36 + 14 + 3 * 36 + 10;
    float bh = 48;
    float bx = std::clamp(anchor.cx() - bw / 2, 12.0f, w - bw - 12);
    float by = anchor.bottom() + 14;
    if (by + bh > h - 12) by = anchor.y - bh - 14;
    if (by < 12) by = std::min(h - bh - 12, anchor.bottom() - bh - 14);
    Rect bar{bx, by, bw, bh};
    shadow(cr, bar, 24, 0.6f, 14);
    fill(cr, bar, 24, p.island.with_alpha(0.97f));
    stroke(cr, bar, 24, p.text.with_alpha(0.08f), 1);
    ui_.add(ID("cap.bar"), bar).cursor_shape(CursorShape::Default);

    float x = bar.x + 10, cy = bar.cy();
    std::string tip;
    for (int i = 0; i < 10; ++i) {
        Rect r{x, cy - 16, 32, 32};
        uint64_t id = ID("cap.tool", i);
        bool on = tool_ == tools[i].t;
        float hv = ui_.hover(id);
        if (on) fill(cr, r, 10, p.accent);
        else if (hv > 0.01f) fill(cr, r, 10, p.text.with_alpha(0.08f * hv));
        tool_icon(cr, tools[i].t, r.cx(), r.cy(), on ? p.on_accent : mix(p.text_dim, p.text, hv));
        if (hv > 0.5f) tip = tools[i].tip;
        Tool t = tools[i].t;
        ui_.add(id, r).click([this, t] {
            if (typing_) commit_text();
            tool_ = t;
        });
        x += 36;
    }
    x += 6;
    divider(cr, x, cy - 14, x, cy + 14, p.text.with_alpha(0.12f));
    x += 8;
    for (int i = 0; i < kColors; ++i) {
        uint64_t id = ID("cap.color", i);
        float hv = ui_.hover(id);
        float r = 7 + hv * 1.5f + (color_ == i ? 1.5f : 0);
        cairo_arc(cr, x + 10, cy, r, 0, 2 * M_PI);
        set(cr, kPalette[i]);
        cairo_fill(cr);
        if (color_ == i) {
            cairo_arc(cr, x + 10, cy, r + 3.5, 0, 2 * M_PI);
            set(cr, p.text);
            cairo_set_line_width(cr, 1.6);
            cairo_stroke(cr);
        } else if (i == kColors - 1) {
            cairo_arc(cr, x + 10, cy, r, 0, 2 * M_PI);
            set(cr, p.text.with_alpha(0.35f));
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        ui_.add(id, {x, cy - 14, 20, 28}).click([this, i] {
            color_ = i;
            if (selected_ >= 0 && selected_ < static_cast<int>(shapes_.size())) {
                push_undo();
                shapes_[selected_].color = kPalette[i];
            }
        });
        x += 22;
    }
    x += 6;
    divider(cr, x, cy - 14, x, cy + 14, p.text.with_alpha(0.12f));
    x += 8;
    {
        // Stroke width: a dot that grows; click cycles, scroll adjusts.
        uint64_t id = ID("cap.width");
        Rect r{x, cy - 16, 32, 32};
        float hv = ui_.hover(id);
        if (hv > 0.01f) fill(cr, r, 10, p.text.with_alpha(0.08f * hv));
        float d = std::clamp(width_ * 0.9f + 2, 3.0f, 20.0f);
        fill(cr, {r.cx() - d / 2, r.cy() - d / 2, d, d}, d / 2, kPalette[color_]);
        if (hv > 0.5f) tip = "Grosor " + std::to_string(static_cast<int>(width_)) + " (rueda)";
        ui_.add(id, r)
            .click([this] { width_ = width_ < 3.5f ? 4 : width_ < 7.5f ? 8 : 2; })
            .scroll([this](float dy) { width_ = std::clamp(width_ - dy, 1.0f, 30.0f); });
        x += 36;
    }
    {
        uint64_t id = ID("cap.fill");
        Rect r{x, cy - 16, 32, 32};
        float hv = ui_.hover(id);
        if (fill_) fill(cr, r, 10, p.accent_dim);
        else if (hv > 0.01f) fill(cr, r, 10, p.text.with_alpha(0.08f * hv));
        rrect(cr, r.inset(9), 3);
        if (fill_) {
            set(cr, kPalette[color_].with_alpha(0.6f));
            cairo_fill_preserve(cr);
        }
        set(cr, mix(p.text_dim, p.text, hv));
        cairo_set_line_width(cr, 1.6);
        cairo_stroke(cr);
        if (hv > 0.5f) tip = "Relleno (F)";
        ui_.add(id, r).click([this] { fill_ = !fill_; });
        x += 36;
    }
    x += 6;
    divider(cr, x, cy - 14, x, cy + 14, p.text.with_alpha(0.12f));
    x += 8;
    w::icon_button(c, ID("cap.undo"), {x, cy - 16, 32, 32}, "󰕌", 15, false, undo_.empty() ? p.text_faint : Color{})
        .click([this] { undo(); });
    if (ui_.hover(ID("cap.undo")) > 0.5f) tip = "Deshacer (Ctrl+Z)";
    x += 36;
    w::icon_button(c, ID("cap.redo"), {x, cy - 16, 32, 32}, "󰑎", 15, false, redo_.empty() ? p.text_faint : Color{})
        .click([this] { redo(); });
    if (ui_.hover(ID("cap.redo")) > 0.5f) tip = "Rehacer (Ctrl+Shift+Z)";
    x += 36;
    x += 6;
    divider(cr, x, cy - 14, x, cy + 14, p.text.with_alpha(0.12f));
    x += 8;
    w::icon_button(c, ID("cap.copy"), {x, cy - 16, 32, 32}, "󰆏", 15).click([this] { finish(false, true); });
    if (ui_.hover(ID("cap.copy")) > 0.5f) tip = "Copiar (Ctrl+C)";
    x += 36;
    w::icon_button(c, ID("cap.save"), {x, cy - 16, 32, 32}, "󰆓", 15, true).click([this] {
        finish(true, config().screenshot_copy);
    });
    if (ui_.hover(ID("cap.save")) > 0.5f) tip = "Guardar (Enter)";
    x += 36;
    w::icon_button(c, ID("cap.close"), {x, cy - 16, 32, 32}, "󰅖", 15).click([this] { close(); });
    if (ui_.hover(ID("cap.close")) > 0.5f) tip = "Cancelar (Esc)";

    if (!tip.empty()) {
        Text tt{ui(11.5f, "SemiBold"), Color{1, 1, 1, 1}};
        Size ts = measure(tip, tt);
        float ty = bar.y > anchor.bottom() ? bar.bottom() + 8 : bar.y - 8 - 24;
        Rect tr{std::clamp(ui_.px() - ts.w / 2 - 10, 8.0f, w - ts.w - 28), ty, ts.w + 20, 24};
        fill(cr, tr, 12, Color{0, 0, 0, 0.8f});
        text(cr, tr.x + 10, tr.y + 4, tip, tt);
    }
}

void Capture::draw_picker_bar(cairo_t* cr, const Rect& anchor, float w, float h) {
    const auto& p = T();
    w::Ctx c{cr, ui_};
    float bw = 420, bh = 52;
    float bx = std::clamp(anchor.cx() - bw / 2, 12.0f, w - bw - 12);
    float by = anchor.bottom() + 14;
    if (by + bh > h - 12) by = std::max(12.0f, anchor.y - bh - 14);
    Rect bar{bx, by, bw, bh};
    shadow(cr, bar, 26, 0.6f, 14);
    fill(cr, bar, 26, p.island.with_alpha(0.97f));
    ui_.add(ID("pick.bar"), bar).cursor_shape(CursorShape::Default);
    bool ok = has_sel_ && sel_.w > 8 && sel_.h > 8;
    w::pill(c, ID("pick.rec"), {bar.x + 8, bar.y + 8, 180, 36}, "Grabar esta zona", "󰑋",
            ok ? w::Style::Danger : w::Style::Ghost)
        .click([this, ok] {
            if (!ok) return;
            auto cb = region_done_;
            Rect r = sel_;
            close();
            if (cb) cb(r);
        });
    w::pill(c, ID("pick.full"), {bar.x + 194, bar.y + 8, 130, 36}, "Pantalla", "󰍹", w::Style::Plain).click([this] {
        auto cb = region_done_;
        Rect r{0, 0, width() / vs_, height() / vs_};
        close();
        if (cb) cb(r);
    });
    w::pill(c, ID("pick.cancel"), {bar.x + 330, bar.y + 8, 82, 36}, "Cancelar", nullptr, w::Style::Ghost).click([this] {
        auto cb = region_cancel_;
        close();
        if (cb) cb();
    });
}

void Capture::draw_loupe(cairo_t* cr, float x, float y, float w, float h) {
    if (!img_) return;
    float ix = to_ix(x), iy = to_iy(y);
    if (ix < 0 || iy < 0 || ix >= iw_ || iy >= ih_) return;
    float R = 62, zoom = 8;
    float lx = x + 30 + R, ly = y + 30 + R;
    if (lx + R > w - 8) lx = x - 30 - R;
    if (ly + R + 30 > h - 8) ly = y - 30 - R - 24;
    cairo_save(cr);
    cairo_arc(cr, lx, ly, R, 0, 2 * M_PI);
    cairo_clip(cr);
    cairo_translate(cr, lx, ly);
    cairo_scale(cr, zoom * vs_, zoom * vs_);
    cairo_translate(cr, -ix - 0.5, -iy - 0.5);
    cairo_set_source_surface(cr, img_, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
    cairo_set_line_width(cr, 2.5);
    cairo_arc(cr, lx, ly, R, 0, 2 * M_PI);
    cairo_stroke(cr);
    // centre pixel
    cairo_set_source_rgba(cr, 1, 0.25, 0.25, 0.95);
    cairo_set_line_width(cr, 1.2);
    cairo_rectangle(cr, lx - zoom / 2, ly - zoom / 2, zoom, zoom);
    cairo_stroke(cr);
    // coordinates and colour under the pointer
    cairo_surface_flush(img_);
    const unsigned char* d = cairo_image_surface_get_data(img_);
    int stride = cairo_image_surface_get_stride(img_);
    uint32_t px = reinterpret_cast<const uint32_t*>(d + static_cast<int>(iy) * stride)[static_cast<int>(ix)];
    char info[64];
    snprintf(info, sizeof(info), "%d, %d   #%02X%02X%02X", static_cast<int>(ix), static_cast<int>(iy), (px >> 16) & 0xff,
             (px >> 8) & 0xff, px & 0xff);
    Text t{mono(11, "SemiBold"), Color{1, 1, 1, 1}, Align::Center};
    Size ts = measure(info, t);
    Rect tr{lx - ts.w / 2 - 10, ly + R + 8, ts.w + 20, 22};
    fill(cr, tr, 11, Color{0, 0, 0, 0.78f});
    text(cr, lx, tr.y + 4, info, t);
}

void Capture::draw(cairo_t* cr, float w, float h, float dt) {
    (void)dt;
    const auto& p = T();
    // Image placement: whole-output captures map 1:1 onto the overlay.
    if (img_) {
        if (screen_) {
            vs_ = w / iw_;
            vx_ = vy_ = 0;
        } else {
            vs_ = std::min({1.0f, (w - 160) / iw_, (h - 240) / ih_});
            vx_ = std::round((w - iw_ * vs_) / 2);
            vy_ = std::round((h - ih_ * vs_) / 2 - 20);
        }
    } else {
        vs_ = 1;
        vx_ = vy_ = 0;
    }

    // Background: the frozen capture (or the live screen when picking).
    if (img_) {
        if (!screen_) {
            cairo_set_source_rgba(cr, 0.03, 0.03, 0.04, 0.88);
            cairo_paint(cr);
        }
        cairo_save(cr);
        cairo_translate(cr, vx_, vy_);
        cairo_scale(cr, vs_, vs_);
        cairo_set_source_surface(cr, img_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), screen_ && std::fabs(vs_ - 1) < 0.001f ? CAIRO_FILTER_FAST
                                                                                               : CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        for (size_t i = 0; i < shapes_.size(); ++i) draw_shape(cr, shapes_[i], drag_ == Drag::Draw && i + 1 == shapes_.size());
        cairo_restore(cr);
    }

    // Dim everything outside the selection.
    Rect s = sel_screen();
    cairo_save(cr);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(cr, 0, 0, w, h);
    if (has_sel_) cairo_rectangle(cr, s.x, s.y, s.w, s.h);
    cairo_set_source_rgba(cr, 0, 0, 0, picking_ ? 0.45 : 0.5);
    cairo_fill(cr);
    cairo_restore(cr);

    if (has_sel_ && (s.w > 0 || s.h > 0)) {
        // Border and handles
        cairo_set_source_rgba(cr, 1, 1, 1, 0.95);
        cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, std::round(s.x) - 0.75, std::round(s.y) - 0.75, std::round(s.w) + 1.5, std::round(s.h) + 1.5);
        cairo_stroke(cr);
        if (picking_ || tool_ == Tool::Select || drag_ == Drag::Create) {
            float hx[3] = {s.x, s.cx(), s.right()}, hy[3] = {s.y, s.cy(), s.bottom()};
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) {
                    if (i == 1 && j == 1) continue;
                    Rect hr{hx[i] - 5, hy[j] - 5, 10, 10};
                    fill(cr, hr, 2.5f, Color{1, 1, 1, 1});
                    stroke(cr, hr, 2.5f, p.accent, 1.5f);
                }
        }
        // Size label
        char dim[48];
        snprintf(dim, sizeof(dim), "%d × %d", static_cast<int>(std::lround(sel_.w)), static_cast<int>(std::lround(sel_.h)));
        Text dt_{mono(12, "SemiBold"), Color{1, 1, 1, 1}};
        Size ds = measure(dim, dt_);
        float ly = s.y - 30 >= 4 ? s.y - 30 : s.y + 6;
        Rect lr{s.x, ly, ds.w + 18, 24};
        fill(cr, lr, 12, Color{0, 0, 0, 0.75f});
        text(cr, lr.x + 9, lr.y + 4, dim, dt_);
    }

    // Selected annotation outline
    if (selected_ >= 0 && selected_ < static_cast<int>(shapes_.size())) {
        Rect b = shapes_[selected_].bounds().inset(-(shapes_[selected_].width / 2 + 5));
        Rect sb{to_sx(b.x), to_sy(b.y), b.w * vs_, b.h * vs_};
        cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
        double dash[] = {5, 4};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_line_width(cr, 1.2);
        cairo_rectangle(cr, sb.x, sb.y, sb.w, sb.h);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }

    // Text being typed
    if (typing_) {
        Shape preview;
        preview.kind = Shape::Text;
        preview.pts = {text_x_, text_y_};
        preview.color = kPalette[color_];
        preview.width = width_;
        preview.text = text_.text;
        cairo_save(cr);
        cairo_translate(cr, vx_, vy_);
        cairo_scale(cr, vs_, vs_);
        if (!text_.text.empty()) draw_shape(cr, preview, true);
        Rect caret = caret_rect(text_.text, {ui(width_ * 5 + 12, "Bold")}, text_.caret);
        if (std::fmod(now_mono(), 1.0) < 0.6)
            fill(cr, {text_x_ + caret.x, text_y_ + caret.y, 2.5f, std::max(caret.h, width_ * 5 + 12)}, 1, kPalette[color_]);
        cairo_restore(cr);
        request();
    }

    // Help before anything is selected
    if (!has_sel_ || (drag_ == Drag::Create && (sel_.w < 4 || sel_.h < 4))) {
        std::string msg = picking_ ? "Arrastra para elegir la zona a grabar  ·  clic = pantalla completa  ·  Esc cancela"
                                   : "Arrastra para seleccionar  ·  clic = todo  ·  Esc cancela";
        Text t{ui(14, "SemiBold"), Color{1, 1, 1, 1}, Align::Center};
        Size ts = measure(msg, t);
        Rect r{w / 2 - ts.w / 2 - 18, 40, ts.w + 36, 38};
        fill(cr, r, 19, Color{0, 0, 0, 0.72f});
        text(cr, w / 2, r.y + 10, msg, t);
    }

    // Loupe while choosing the region
    if (img_ && pointer_in_ && (!has_sel_ || drag_ == Drag::Create || drag_ == Drag::Resize)) draw_loupe(cr, px_, py_, w, h);

    // Toolbars once a region exists and is not being dragged
    bool dragging_sel = drag_ == Drag::Create || drag_ == Drag::Move || drag_ == Drag::Resize;
    if (has_sel_ && !dragging_sel && sel_.w >= 4 && sel_.h >= 4) {
        if (picking_) draw_picker_bar(cr, s, w, h);
        else if (img_) draw_toolbar(cr, s, w, h);
    }
}

}  // namespace island
