#pragma once

#include <string>

#include "core/util.hpp"

namespace island {

// Design tokens. Views never hardcode a colour: everything reads Theme, so a
// new wallpaper palette repaints the whole island (with a short cross-fade).
struct Palette {
    Color island;        // the island body
    Color surface;       // cards on the island
    Color surface_hi;    // hovered card / control track
    Color border;        // hairlines
    Color text;
    Color text_dim;
    Color text_faint;
    Color accent;
    Color accent_dim;    // accent container (active tiles, slider fill)
    Color on_accent;
    Color good;
    Color warn;
    Color bad;
    Color info;
};

class Theme {
public:
    static Theme& get();

    const Palette& p() const { return cur_; }

    // Loads the configured palette (adaptive reads the wallpaper colours file).
    void load();
    // Advances the cross-fade; returns true while it is still moving.
    bool update(float dt);
    std::string palette_path() const;

    // A palette built around one accent on a dark ground.
    static Palette from_accent(Color accent, Color ground, Color text);
    static Palette named(const std::string& name);

private:
    Palette cur_{}, from_{}, to_{};
    float t_ = 1.0f;
    bool first_ = true;
    void set_target(const Palette& p);
};

inline const Palette& T() { return Theme::get().p(); }

}  // namespace island
