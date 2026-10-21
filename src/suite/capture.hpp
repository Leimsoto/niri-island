#pragma once

#include <cairo.h>
#include <functional>
#include <string>
#include <vector>

#include "suite/overlay.hpp"
#include "ui/textfield.hpp"

namespace island {

struct Shape {
    enum Kind { Rect, Ellipse, Arrow, Line, Pen, Marker, Text, Blur, Counter } kind = Rect;
    std::vector<float> pts;  // x0,y0,x1,y1,… in image pixels
    Color color;
    float width = 4;
    bool filled = false;
    std::string text;
    int number = 0;
    island::Rect bounds() const;
};

// The screenshot editor: a frozen capture with a region you can draw, move
// and resize, annotation tools, undo/redo, save and copy. The same overlay,
// without an image, is the live region picker for the screen recorder.
class Capture : public Overlay {
public:
    enum class Tool { Select, Rect, Ellipse, Arrow, Line, Pen, Marker, Text, Blur, Counter };

    explicit Capture(Wayland& wl) : Overlay(wl) {}
    ~Capture() override;

    // Takes ownership of `img` (image pixels). `screen` means it is a capture
    // of the whole output, shown 1:1; otherwise it is fitted and centred.
    void edit(cairo_surface_t* img, bool select_all, bool screen);
    void select_region(std::function<void(const island::Rect&)> done, std::function<void()> cancelled);

    // Encodes, saves to the screenshots folder and/or copies, off the main
    // thread; `done` gets the saved path ("" if not saved) and a thumbnail.
    static void save_async(cairo_surface_t* out, bool save, bool copy,
                           std::function<void(const std::string& path, cairo_surface_t* thumb)> done);

    // path is empty when only copied.
    std::function<void(const std::string& path, cairo_surface_t* thumb, bool copied)> on_saved;
    std::function<void(const std::string& msg)> on_toast;
    // A short message over the editor (the island's toasts are under it).
    void notice(const std::string& msg) {
        notice_ = msg;
        notice_at_ = now_mono();
        request();
    }

protected:
    void draw(cairo_t* cr, float w, float h, float dt) override;
    void key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) override;
    void canvas_button(Button b, bool pressed, float x, float y) override;
    void canvas_motion(float x, float y) override;
    void canvas_scroll(float dy) override;
    CursorShape canvas_cursor() const override;
    void closed() override;
    const char* name() const override { return "niri-island-capture"; }

private:
    std::string notice_;
    double notice_at_ = -10;
    enum class Drag { None, Create, Move, Resize, Draw, ShapeMove };
    bool picking_ = false;  // region picker for the recorder
    cairo_surface_t* img_ = nullptr;
    int iw_ = 0, ih_ = 0;
    bool screen_ = true;
    float vs_ = 1, vx_ = 0, vy_ = 0;  // image → screen
    island::Rect sel_{};
    bool has_sel_ = false;
    Drag drag_ = Drag::None;
    int handle_ = -1;
    float ax_ = 0, ay_ = 0;
    island::Rect start_sel_{};
    std::vector<float> start_pts_;
    Tool tool_ = Tool::Arrow;
    int color_ = 0;
    float width_ = 4;
    bool fill_ = false;
    std::vector<Shape> shapes_;
    std::vector<std::vector<Shape>> undo_, redo_;
    int selected_ = -1;
    bool typing_ = false;
    TextField text_;
    float text_x_ = 0, text_y_ = 0;
    std::function<void(const island::Rect&)> region_done_;
    std::function<void()> region_cancel_;
    bool busy_ = false;

    float to_sx(float x) const { return vx_ + x * vs_; }
    float to_sy(float y) const { return vy_ + y * vs_; }
    float to_ix(float x) const { return (x - vx_) / vs_; }
    float to_iy(float y) const { return (y - vy_) / vs_; }
    island::Rect sel_screen() const { return {to_sx(sel_.x), to_sy(sel_.y), sel_.w * vs_, sel_.h * vs_}; }
    int handle_at(float x, float y) const;
    int shape_at(float ix, float iy) const;
    void push_undo();
    void undo();
    void redo();
    void commit_text();
    void finish(bool save, bool copy);
    void reset();
    cairo_surface_t* render_result();
    void draw_shape(cairo_t* cr, const Shape& s, bool preview) const;
    void draw_toolbar(cairo_t* cr, const island::Rect& anchor, float w, float h);
    void draw_picker_bar(cairo_t* cr, const island::Rect& anchor, float w, float h);
    void draw_loupe(cairo_t* cr, float x, float y, float w, float h);
    static void tool_icon(cairo_t* cr, Tool t, float cx, float cy, Color c);
};

}  // namespace island
