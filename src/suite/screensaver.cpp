#include "suite/screensaver.hpp"

#include <cmath>
#include <random>

#include "core/loop.hpp"

namespace island {

namespace {

Color hue_color(float h) {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360;
    float s = 0.62f, v = 1.0f;
    float c = v * s, x = c * (1 - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) r = c, g = x;
    else if (h < 120) r = x, g = c;
    else if (h < 180) g = c, b = x;
    else if (h < 240) g = x, b = c;
    else if (h < 300) r = x, b = c;
    else r = c, b = x;
    return {r + m, g + m, b + m, 1};
}

}  // namespace

Screensaver::~Screensaver() {
    for (auto& [k, s] : sprites_) cairo_surface_destroy(s);
}

void Screensaver::start() {
    if (exists()) return;
    open();
}

void Screensaver::opened() {
    bubbles_.clear();
    t_ = 0;
    next_ghost_ = 8;
    px0_ = py0_ = -1;
    opened_at_ = now_mono();
}

void Screensaver::canvas_motion(float x, float y) {
    // The first position is where the pointer already was: only a real move ends it.
    if (px0_ < 0 || now_mono() - opened_at_ < 0.8) {
        px0_ = x;
        py0_ = y;
        return;
    }
    if (std::hypot(x - px0_, y - py0_) > 12) stop();
}

void Screensaver::spawn(float w, float h) {
    static std::mt19937 rng(static_cast<unsigned>(now_mono() * 1000));
    std::uniform_real_distribution<float> U(0, 1);
    // all the same size, large, like the original
    float base = std::round(std::min(w, h) * 0.068f);
    int n = std::clamp(static_cast<int>(w * h / (base * base * 12.0f)), 10, 40);
    // Blown out of one spot, like a breath into a bubble wand: a fan of
    // bubbles, fast at first, slowing to a drift.
    // from one edge (any of the four), the line heads across the screen
    int side = static_cast<int>(U(rng) * 4) % 4;
    float ox = side == 0 ? base : side == 1 ? w - base : w * (0.2f + 0.6f * U(rng));
    float oy = side == 2 ? h - base : side == 3 ? base : h * (0.25f + 0.5f * U(rng));
    float aim = std::atan2(h / 2 - oy, w / 2 - ox) + (U(rng) - 0.5f) * 0.6f;
    // They come out in a line, one after another along the same path, then
    // each bends off on its own curve and they scatter.
    float puff = base * 14.0f;
    for (int i = 0; i < n; ++i) {
        Bubble b;
        b.r = base;
        b.x = ox;
        b.y = oy;
        float ang = aim + (U(rng) - 0.5f) * 0.08f;
        b.cruise = base * (0.8f + 0.7f * U(rng));
        b.vx = std::cos(ang) * puff;
        b.vy = std::sin(ang) * puff;
        b.turn = (0.7f + 1.5f * U(rng)) * (U(rng) < 0.5f ? -1.0f : 1.0f);
        b.hue = U(rng) * 360;
        b.hue_speed = (6 + 10 * U(rng)) * (U(rng) < 0.5f ? -1 : 1);
        b.born = i * (2.1f * base / puff);  // one diameter apart along the line
        bubbles_.push_back(b);
    }
}

// One bubble, drawn once per hue step: thin coloured film, a bright rim, an
// inner reflection, a specular highlight and a soft shadow underneath.
cairo_surface_t* Screensaver::sprite(float r, float hue) {
    int step = static_cast<int>(std::lround(std::fmod(hue + 360, 360.0f) / 6)) % 60;
    int key = step * 1000 + static_cast<int>(r);
    auto it = sprites_.find(key);
    if (it != sprites_.end()) return it->second;
    int sc = std::max(1, scale());
    float pad = r * 0.35f;
    int size = static_cast<int>(std::ceil((2 * r + 2 * pad) * sc));
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t* c = cairo_create(s);
    cairo_scale(c, sc, sc);
    float cx = r + pad, cy = r + pad;
    Color col = hue_color(step * 6.0f);
    cairo_set_antialias(c, CAIRO_ANTIALIAS_BEST);
    // soft shadow, a little down and right
    {
        float sx = cx + r * 0.10f, sy = cy + r * 0.16f;
        cairo_pattern_t* g = cairo_pattern_create_radial(sx, sy, r * 0.75, sx, sy, r * 1.12);
        cairo_pattern_add_color_stop_rgba(g, 0, 0, 0, 0, 0.16);
        cairo_pattern_add_color_stop_rgba(g, 1, 0, 0, 0, 0);
        cairo_arc(c, sx, sy, r * 1.12, 0, 2 * M_PI);
        cairo_set_source(c, g);
        cairo_fill(c);
        cairo_pattern_destroy(g);
    }
    // the film: the colour sits on the edge and fades evenly inward, all round
    {
        cairo_pattern_t* g = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r);
        cairo_pattern_add_color_stop_rgba(g, 0, col.r, col.g, col.b, 0.0);
        cairo_pattern_add_color_stop_rgba(g, 0.55, col.r, col.g, col.b, 0.03);
        cairo_pattern_add_color_stop_rgba(g, 0.78, col.r, col.g, col.b, 0.12);
        cairo_pattern_add_color_stop_rgba(g, 0.92, col.r, col.g, col.b, 0.38);
        cairo_pattern_add_color_stop_rgba(g, 1, col.r, col.g, col.b, 0.78);
        cairo_arc(c, cx, cy, r, 0, 2 * M_PI);
        cairo_set_source(c, g);
        cairo_fill(c);
        cairo_pattern_destroy(g);
    }
    // specular highlight: a clean, sharp white ellipse, with a faint halo
    cairo_save(c);
    cairo_translate(c, cx - r * 0.47f, cy - r * 0.50f);
    cairo_rotate(c, -0.72);
    cairo_save(c);
    cairo_scale(c, 1.0, 0.55);
    {
        cairo_pattern_t* g = cairo_pattern_create_radial(0, 0, 0, 0, 0, r * 0.3);
        cairo_pattern_add_color_stop_rgba(g, 0, 1, 1, 1, 0.22);
        cairo_pattern_add_color_stop_rgba(g, 1, 1, 1, 1, 0);
        cairo_arc(c, 0, 0, r * 0.3, 0, 2 * M_PI);
        cairo_set_source(c, g);
        cairo_fill(c);
        cairo_pattern_destroy(g);
    }
    cairo_restore(c);
    cairo_scale(c, 1.0, 0.48);
    cairo_arc(c, 0, 0, r * 0.15, 0, 2 * M_PI);
    cairo_set_source_rgba(c, 1, 1, 1, 0.95);
    cairo_fill(c);
    cairo_restore(c);
    cairo_destroy(c);
    cairo_surface_set_device_scale(s, sc, sc);
    sprites_[key] = s;
    return s;
}

void Screensaver::draw(cairo_t* cr, float w, float h, float dt) {
    if (w_ != w || h_ != h || bubbles_.empty()) {
        w_ = w;
        h_ = h;
        bubbles_.clear();
        spawn(w, h);
    }
    dt = std::min(dt, 0.033f);
    t_ += dt;
    // Move, bounce off the edges, then off each other (equal masses, elastic).
    for (auto& b : bubbles_) {
        if (t_ < b.born) continue;
        // straight out of the line at first, then the path bends away
        float age = t_ - b.born;
        if (age > 0.35f && age < 2.0f) {
            float k = std::min(1.0f, (age - 0.35f) / 0.4f) * (1.0f - std::max(0.0f, age - 1.5f) / 0.5f);
            float a = b.turn * k * dt, ca = std::cos(a), sa = std::sin(a);
            float vx = b.vx * ca - b.vy * sa, vy = b.vx * sa + b.vy * ca;
            b.vx = vx;
            b.vy = vy;
        }
        // the puff dies down to a drift
        float sp = std::hypot(b.vx, b.vy);
        if (sp > b.cruise * 1.01f) {
            float k = std::max(b.cruise / sp, std::exp(-1.15f * dt));
            b.vx *= k;
            b.vy *= k;
        }
        b.x += b.vx * dt;
        b.y += b.vy * dt;
        if (b.x < b.r) b.x = b.r, b.vx = std::fabs(b.vx);
        if (b.x > w - b.r) b.x = w - b.r, b.vx = -std::fabs(b.vx);
        if (b.y < b.r) b.y = b.r, b.vy = std::fabs(b.vy);
        if (b.y > h - b.r) b.y = h - b.r, b.vy = -std::fabs(b.vy);
        b.hue += b.hue_speed * dt;
    }
    // Ghosts pass through; once a bubble has been clear of the others for a
    // moment it becomes solid and bounces from then on.
    std::vector<bool> touching(bubbles_.size(), false);
    for (size_t i = 0; i < bubbles_.size(); ++i)
        for (size_t j = i + 1; j < bubbles_.size(); ++j) {
            Bubble &a = bubbles_[i], &b = bubbles_[j];
            if (t_ < a.born || t_ < b.born) continue;
            float dx = b.x - a.x, dy = b.y - a.y, d = std::hypot(dx, dy), min = a.r + b.r;
            if (d >= min || d < 1e-3f) continue;
            touching[i] = touching[j] = true;
            if (a.ghost || b.ghost) continue;
            float nx = dx / d, ny = dy / d;
            // eased apart, not snapped (two that were inside each other glide out)
            float push = std::min((min - d) / 2, a.r * 2.5f * dt);
            a.x -= nx * push, a.y -= ny * push;
            b.x += nx * push, b.y += ny * push;
            float rel = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny;
            if (rel < 0) {
                a.vx += rel * nx, a.vy += rel * ny;
                b.vx -= rel * nx, b.vy -= rel * ny;
            }
        }
    // Rarely, one bubble turns ghost for a couple of seconds and drifts
    // through whatever it meets; then it is solid again (once it is clear).
    if (t_ > next_ghost_ && !bubbles_.empty()) {
        static std::mt19937 rng(static_cast<unsigned>(now_mono() * 997));
        std::uniform_real_distribution<float> U(0, 1);
        Bubble& g = bubbles_[static_cast<size_t>(U(rng) * bubbles_.size()) % bubbles_.size()];
        if (!g.ghost) {
            g.ghost = true;
            g.ghost_until = t_ + 1.5f + 1.5f * U(rng);
            g.clear_t = 0;
        }
        next_ghost_ = t_ + 7.0f + 8.0f * U(rng);
    }
    for (size_t i = 0; i < bubbles_.size(); ++i) {
        Bubble& b = bubbles_[i];
        if (!b.ghost) continue;
        b.clear_t = touching[i] ? 0.0f : b.clear_t + dt;
        if (t_ > b.ghost_until && (b.clear_t > 0.2f || t_ > b.ghost_until + 2.5f)) b.ghost = false;
    }
    // Over the desktop, slightly dimmed.
    cairo_set_source_rgba(cr, 0, 0, 0, std::min(0.28f, t_ * 0.4f));
    cairo_paint(cr);
    for (auto& b : bubbles_) {
        if (t_ < b.born) continue;
        float grow = std::min(1.0f, 0.08f + (t_ - b.born) / 0.7f);
        grow = 1 - (1 - grow) * (1 - grow);
        cairo_surface_t* s = sprite(b.r, b.hue);
        float pad = b.r * 0.35f, full = b.r + pad;
        cairo_save(cr);
        cairo_translate(cr, b.x, b.y);
        if (grow < 1) cairo_scale(cr, grow, grow);
        cairo_set_source_surface(cr, s, -full, -full);
        cairo_paint_with_alpha(cr, grow);
        cairo_restore(cr);
    }
}

}  // namespace island
