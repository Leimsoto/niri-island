#include "ui/textfield.hpp"

#include <cctype>

#include "core/util.hpp"
#include "core/wayland.hpp"

namespace island {

static bool is_word(unsigned char c) { return c >= 0x80 || std::isalnum(c) || c == '_'; }

void TextField::insert(const std::string& s) {
    std::string clean;
    for (char c : s) {
        if (c == '\r') continue;
        if (c == '\n' && !multiline) {
            clean += ' ';
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') continue;
        clean += c;
    }
    if (text.size() + clean.size() > max_bytes) clean.resize(max_bytes > text.size() ? max_bytes - text.size() : 0);
    caret = std::min(caret, text.size());
    text.insert(caret, clean);
    caret += clean.size();
}

bool TextField::backspace(bool word) {
    if (caret == 0) return false;
    size_t start = utf8_prev(text, caret);
    if (word) {
        while (start > 0 && !is_word(static_cast<unsigned char>(text[start]))) start = utf8_prev(text, start);
        while (start > 0 && is_word(static_cast<unsigned char>(text[utf8_prev(text, start)])))
            start = utf8_prev(text, start);
    }
    text.erase(start, caret - start);
    caret = start;
    return true;
}

bool TextField::del() {
    if (caret >= text.size()) return false;
    size_t next = utf8_next(text, caret);
    text.erase(caret, next - caret);
    return true;
}

void TextField::left(bool word) {
    if (caret == 0) return;
    caret = utf8_prev(text, caret);
    if (word) {
        while (caret > 0 && !is_word(static_cast<unsigned char>(text[caret]))) caret = utf8_prev(text, caret);
        while (caret > 0 && is_word(static_cast<unsigned char>(text[utf8_prev(text, caret)])))
            caret = utf8_prev(text, caret);
    }
}

void TextField::right(bool word) {
    if (caret >= text.size()) return;
    caret = utf8_next(text, caret);
    if (word) {
        while (caret < text.size() && is_word(static_cast<unsigned char>(text[caret]))) caret = utf8_next(text, caret);
    }
}

void TextField::home() {
    if (!multiline) {
        caret = 0;
        return;
    }
    size_t nl = caret == 0 ? std::string::npos : text.rfind('\n', caret - 1);
    caret = nl == std::string::npos ? 0 : nl + 1;
}

void TextField::end() {
    if (!multiline) {
        caret = text.size();
        return;
    }
    size_t nl = text.find('\n', caret);
    caret = nl == std::string::npos ? text.size() : nl;
}

TextField::Result TextField::key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods) {
    bool ctrl = mods & Wayland::MOD_CTRL;
    switch (sym) {
        case XKB_KEY_Escape: return Result::Cancel;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (multiline && !(mods & Wayland::MOD_CTRL)) {
                insert("\n");
                return Result::Changed;
            }
            return Result::Submit;
        case XKB_KEY_BackSpace: return backspace(ctrl) ? Result::Changed : Result::None;
        case XKB_KEY_Delete: return del() ? Result::Changed : Result::None;
        case XKB_KEY_Left: left(ctrl); return Result::Moved;
        case XKB_KEY_Right: right(ctrl); return Result::Moved;
        case XKB_KEY_Home: home(); return Result::Moved;
        case XKB_KEY_End: end(); return Result::Moved;
        case XKB_KEY_Up: return Result::Up;
        case XKB_KEY_Down: return Result::Down;
        case XKB_KEY_Tab:
        case XKB_KEY_ISO_Left_Tab: return Result::Tab;
        default: break;
    }
    if (ctrl) {
        if (sym == XKB_KEY_v || sym == XKB_KEY_V) return Result::Paste;
        if (sym == XKB_KEY_u || sym == XKB_KEY_U) {
            clear();
            return Result::Changed;
        }
        if (sym == XKB_KEY_a || sym == XKB_KEY_A) {
            home();
            return Result::Moved;
        }
        if (sym == XKB_KEY_e || sym == XKB_KEY_E) {
            end();
            return Result::Moved;
        }
        if (sym == XKB_KEY_w || sym == XKB_KEY_W) return backspace(true) ? Result::Changed : Result::None;
        return Result::None;
    }
    if (!utf8.empty() && static_cast<unsigned char>(utf8[0]) >= 0x20 && utf8[0] != 0x7f) {
        insert(utf8);
        return Result::Changed;
    }
    return Result::None;
}

}  // namespace island
