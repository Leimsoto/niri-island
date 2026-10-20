#include "island/edgebar.hpp"

#include "core/config.hpp"

namespace island {

void EdgeBar::adjust_spec(LayerSpec& s) const {
    s.layer = layer();
    s.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | (left_ ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    s.width = kWidth;
    s.height = kHeight;
    bool attached = config().island_style == "attached";
    s.margin_top = attached ? 0 : config().margin_top;
    s.margin_side = 0;  // pills are drawn inset (edge_inset()), so their shadow has room
    // Ignore other reserved zones (the island's among them), so the strips sit
    // at the same height as the island instead of below it.
    s.exclusive_zone = -1;
    s.keyboard = KeyboardMode::None;
    s.input_everywhere = false;
}

int EdgeBar::edge_inset() {
    return config().island_style == "attached" && config().dock_sides ? 12 : 16;
}

void EdgeBar::restyle() {
    if (!surf_) return;
    LayerSpec s;
    adjust_spec(s);
    surf_->set_margin(s.margin_top, s.margin_side);
    input_.clear();
    request();
}

void EdgeBar::draw(cairo_t* cr, float w, float h, float) {
    std::vector<Rect> input;
    if (painter) painter(cr, ui_, w, h, left_, input);
    bool changed = input.size() != input_.size();
    for (size_t i = 0; !changed && i < input.size(); ++i)
        changed = std::abs(input[i].x - input_[i].x) > 0.5f || std::abs(input[i].w - input_[i].w) > 0.5f;
    if (changed && surf_) {
        surf_->set_input_rects(input);
        input_ = input;
        std::vector<LayerSurface::BlurShape> blur;
        if (config().glass && config().compositor_blur && wl_.can_blur())
            for (auto& r : input) blur.push_back({r, r.h / 2, config().island_style == "attached" && config().dock_sides});
        surf_->set_blur(blur);
    }
}

}  // namespace island
