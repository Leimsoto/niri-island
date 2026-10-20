#include "ui/theme.hpp"

#include <nlohmann/json.hpp>

#include "core/config.hpp"

namespace island {

using json = nlohmann::json;

Theme& Theme::get() {
    static Theme t;
    return t;
}

static Color darken(Color c, float k) { return mix(c, Color{0, 0, 0, c.a}, k); }
static Color lighten(Color c, float k) { return mix(c, Color{1, 1, 1, c.a}, k); }

Palette Theme::from_accent(Color accent, Color ground, Color text) {
    Palette p;
    // The island stays near-black whatever the wallpaper: it is the shell's
    // identity. Only a trace of the ground's hue survives.
    p.island = mix(Color::hex(0x0b0b0e), ground, 0.35f).with_alpha(0.985f);
    p.surface = mix(p.island, text, 0.055f).with_alpha(0.96f);
    p.surface_hi = mix(p.island, text, 0.11f).with_alpha(0.97f);
    p.border = text.with_alpha(0.085f);
    p.text = text.with_alpha(0.97f);
    p.text_dim = text.with_alpha(0.62f);
    p.text_faint = text.with_alpha(0.38f);
    // Keep the accent readable on black: lift very dark accents.
    if (accent.luminance() < 0.35f) accent = lighten(accent, 0.35f);
    p.accent = accent;
    p.accent_dim = mix(p.island, accent, 0.30f).with_alpha(0.97f);
    p.on_accent = accent.luminance() > 0.55f ? darken(accent, 0.82f) : Color{1, 1, 1, 1};
    p.good = Color::hex(0x5fd38a);
    p.warn = Color::hex(0xf5b84a);
    p.bad = Color::hex(0xff6b5e);
    p.info = Color::hex(0x6db3ff);
    return p;
}

Palette Theme::named(const std::string& name) {
    Color ground = Color::hex(0x141218);
    Color text = Color::hex(0xe8e3ee);
    if (name == "ocean") return from_accent(Color::hex(0x7cc4ff), Color::hex(0x0e1620), text);
    if (name == "rose") return from_accent(Color::hex(0xffa8c5), Color::hex(0x1a1015), text);
    if (name == "mint") return from_accent(Color::hex(0x8ee6c0), Color::hex(0x0f1714), text);
    if (name == "amber") return from_accent(Color::hex(0xffc978), Color::hex(0x18130c), text);
    if (name == "mono") return from_accent(Color::hex(0xe6e6e6), Color::hex(0x101010), Color::hex(0xf2f2f2));
    return from_accent(Color::hex(0xd0bcff), ground, text);  // lavender (Material 3)
}

std::string Theme::palette_path() const { return expand_home(config().palette_file); }

void Theme::load() {
    const auto& cfg = config();
    Palette target = named(cfg.theme);
    if (cfg.theme == "adaptive") {
        std::string text;
        if (read_file(palette_path(), text)) {
            json j = json::parse(text, nullptr, false);
            if (j.is_object()) {
                auto col = [&](const char* key, Color fallback) {
                    Color c;
                    auto it = j.find(key);
                    if (it != j.end() && it->is_string() && Color::parse(it->get<std::string>(), c)) return c;
                    return fallback;
                };
                Color accent = col("primary", Color::hex(0xd0bcff));
                Color ground = col("surface", col("background", Color::hex(0x141218)));
                Color fg = col("surfaceText", Color::hex(0xe8e3ee));
                target = from_accent(accent, ground, fg);
                Color tertiary;
                auto it = j.find("tertiary");
                if (it != j.end() && it->is_string() && Color::parse(it->get<std::string>(), tertiary))
                    target.info = tertiary;
            }
        }
    }
    set_target(target);
}

void Theme::set_target(const Palette& p) {
    if (first_) {
        cur_ = from_ = to_ = p;
        t_ = 1.0f;
        first_ = false;
        return;
    }
    from_ = cur_;
    to_ = p;
    t_ = 0.0f;
}

bool Theme::update(float dt) {
    if (t_ >= 1.0f) return false;
    t_ = std::min(1.0f, t_ + dt / 0.6f);
    float k = smoothstep(t_);
    auto m = [k](const Color& a, const Color& b) { return mix(a, b, k); };
    cur_.island = m(from_.island, to_.island);
    cur_.surface = m(from_.surface, to_.surface);
    cur_.surface_hi = m(from_.surface_hi, to_.surface_hi);
    cur_.border = m(from_.border, to_.border);
    cur_.text = m(from_.text, to_.text);
    cur_.text_dim = m(from_.text_dim, to_.text_dim);
    cur_.text_faint = m(from_.text_faint, to_.text_faint);
    cur_.accent = m(from_.accent, to_.accent);
    cur_.accent_dim = m(from_.accent_dim, to_.accent_dim);
    cur_.on_accent = m(from_.on_accent, to_.on_accent);
    cur_.good = m(from_.good, to_.good);
    cur_.warn = m(from_.warn, to_.warn);
    cur_.bad = m(from_.bad, to_.bad);
    cur_.info = m(from_.info, to_.info);
    return t_ < 1.0f;
}

}  // namespace island
