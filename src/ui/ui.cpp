#include "ui/ui.hpp"

namespace island {

void Ui::begin_frame(float dt) {
    now_ = now_mono();
    last_.swap(cur_);
    cur_.clear();
    xf_.assign(1, {0, 0, 1});
    clips_.clear();
    enabled_ = true;
    update_hot();

    animating_ = false;
    for (auto it = anims_.begin(); it != anims_.end();) {
        Anim& a = it->second;
        a.hover.set(it->first == hot_ ? 1.0f : 0.0f);
        if (it->first == active_) a.press.set(1.0f);
        else a.press.set(0.0f);
        bool m1 = a.hover.update(dt);
        bool m2 = a.press.update(dt);
        animating_ |= m1 || m2;
        if (!m1 && !m2 && a.hover.value == 0.0f && a.press.value == 0.0f && now_ - a.seen > 2.0) {
            it = anims_.erase(it);
        } else {
            ++it;
        }
    }
}

void Ui::push(float ox, float oy, float scale) {
    const Xf& t = xf_.back();
    xf_.push_back({t.ox + ox * t.s, t.oy + oy * t.s, t.s * scale});
}

void Ui::pop() {
    if (xf_.size() > 1) xf_.pop_back();
}

void Ui::push_clip(const Rect& local) {
    const Xf& t = xf_.back();
    Rect r{t.ox + local.x * t.s, t.oy + local.y * t.s, local.w * t.s, local.h * t.s};
    if (!clips_.empty()) r = r.intersect(clips_.back());
    clips_.push_back(r);
}

void Ui::pop_clip() {
    if (!clips_.empty()) clips_.pop_back();
}

Region& Ui::add(uint64_t id, const Rect& local) {
    static Region sink;
    if (!enabled_) {
        sink = Region{};
        return sink;
    }
    const Xf& t = xf_.back();
    Region r;
    r.id = id;
    r.rect = {t.ox + local.x * t.s, t.oy + local.y * t.s, local.w * t.s, local.h * t.s};
    r.clip = clips_.empty() ? Rect{-1e6f, -1e6f, 2e6f, 2e6f} : clips_.back();
    r.ox = t.ox;
    r.oy = t.oy;
    r.scale = t.s;
    cur_.push_back(std::move(r));
    anim(id).seen = now_;
    return cur_.back();
}

Ui::Anim& Ui::anim(uint64_t id) { return anims_[id]; }

float Ui::hover(uint64_t id) {
    auto it = anims_.find(id);
    return it == anims_.end() ? 0.0f : clamp01(it->second.hover.value);
}

float Ui::press(uint64_t id) {
    auto it = anims_.find(id);
    return it == anims_.end() ? 0.0f : std::clamp(it->second.press.value, -0.3f, 1.3f);
}

void Ui::pulse(uint64_t id) {
    Anim& a = anim(id);
    a.press.kick(1.0f);
    a.seen = now_mono();
    animating_ = true;
}

const Region* Ui::hit(float x, float y, bool (*want)(const Region&)) const {
    for (auto it = last_.rbegin(); it != last_.rend(); ++it) {
        if (!it->rect.contains(x, y) || !it->clip.contains(x, y)) continue;
        if (!want || want(*it)) return &*it;
    }
    return nullptr;
}

void Ui::update_hot() {
    uint64_t prev = hot_;
    if (!inside_) {
        hot_ = 0;
    } else if (active_ && active_has_drag_) {
        hot_ = active_;
    } else {
        const Region* r = hit(px_, py_, nullptr);
        hot_ = r ? r->id : 0;
    }
    if (prev != hot_) animating_ = true;
}

void Ui::motion(float x, float y) {
    px_ = x;
    py_ = y;
    inside_ = true;
    update_hot();
    if (active_ && active_has_drag_ && active_copy_.on_drag) {
        // Follow the region as drawn now if it is still there (it may move).
        for (auto& r : last_)
            if (r.id == active_) {
                active_copy_.rect = r.rect;
                break;
            }
        active_copy_.on_drag(drag_event(active_copy_));
    }
}

void Ui::leave() {
    inside_ = false;
    if (!(active_ && active_has_drag_)) {
        hot_ = 0;
    }
}

void Ui::cancel() {
    active_ = 0;
    active_has_drag_ = false;
    active_copy_ = Region{};
}

float Ui::local_x() const {
    const Xf& t = xf_.back();
    return (px_ - t.ox) / t.s;
}

float Ui::local_y() const {
    const Xf& t = xf_.back();
    return (py_ - t.oy) / t.s;
}

void Ui::set_click_local(const Region& r) {
    click_x_ = (px_ - r.ox) / r.scale;
    click_y_ = (py_ - r.oy) / r.scale;
}

DragEvent Ui::drag_event(const Region& r) const {
    DragEvent e;
    e.x = px_;
    e.y = py_;
    e.lx = (px_ - r.ox) / r.scale;
    e.ly = (py_ - r.oy) / r.scale;
    e.rect = r.rect;
    e.fx = r.rect.w > 0 ? (px_ - r.rect.x) / r.rect.w : 0;
    e.fy = r.rect.h > 0 ? (py_ - r.rect.y) / r.rect.h : 0;
    return e;
}

bool Ui::button(Button b, bool pressed) {
    if (b == Button::Left) {
        if (pressed) {
            const Region* r = hit(px_, py_, [](const Region& r) { return bool(r.on_click || r.on_drag); });
            if (!r) return hit(px_, py_, nullptr) != nullptr;  // a plain card swallows the click
            active_ = r->id;
            active_copy_ = *r;
            active_has_drag_ = bool(r->on_drag);
            anim(active_).press.set(1.0f);
            animating_ = true;
            if (active_has_drag_) active_copy_.on_drag(drag_event(active_copy_));
            return true;
        }
        if (!active_) return false;
        Region r = active_copy_;
        bool had_drag = active_has_drag_;
        uint64_t id = active_;
        active_ = 0;
        active_has_drag_ = false;
        active_copy_ = Region{};
        Anim& a = anim(id);
        a.press.set(0.0f);
        animating_ = true;
        if (had_drag) {
            if (r.on_release) r.on_release(drag_event(r));
        } else if (r.rect.contains(px_, py_) && r.on_click) {
            set_click_local(r);
            r.on_click();
        }
        update_hot();
        return true;
    }
    if (!pressed) return false;
    if (b == Button::Right) {
        const Region* r = hit(px_, py_, [](const Region& r) { return bool(r.on_right); });
        if (!r) return false;
        auto f = r->on_right;
        set_click_local(*r);
        pulse(r->id);
        f();
        return true;
    }
    if (b == Button::Middle) {
        const Region* r = hit(px_, py_, [](const Region& r) { return bool(r.on_middle); });
        if (!r) return false;
        auto f = r->on_middle;
        f();
        return true;
    }
    return false;
}

bool Ui::scroll(float dy) {
    const Region* r = hit(px_, py_, [](const Region& r) { return bool(r.on_scroll); });
    if (!r) return false;
    auto f = r->on_scroll;
    f(dy);
    return true;
}

CursorShape Ui::cursor() const {
    if (active_ && active_has_drag_) return active_copy_.cursor == CursorShape::Grab ? CursorShape::Grabbing
                                                                                     : active_copy_.cursor;
    if (!inside_) return CursorShape::Default;
    const Region* r = hit(px_, py_, nullptr);
    if (!r) return CursorShape::Default;
    bool interactive = r->on_click || r->on_drag || r->on_right || r->on_middle;
    return interactive ? r->cursor : (r->cursor == CursorShape::Text ? CursorShape::Text : CursorShape::Default);
}

}  // namespace island
