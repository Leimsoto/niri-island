#pragma once

#include <string>
#include <xkbcommon/xkbcommon.h>

namespace island {

// A small single/multi-line text buffer with a caret, UTF-8 aware.
struct TextField {
    enum class Result { None, Changed, Moved, Submit, Cancel, Paste, Up, Down, Tab };

    std::string text;
    size_t caret = 0;
    bool multiline = false;
    size_t max_bytes = 4096;

    void set(const std::string& t) {
        text = t;
        caret = text.size();
    }
    void clear() {
        text.clear();
        caret = 0;
    }
    void insert(const std::string& s);
    bool backspace(bool word = false);
    bool del();
    void left(bool word = false);
    void right(bool word = false);
    void home();
    void end();

    // Handles one key press. Paste is reported (Ctrl+V) for the app to fetch
    // the clipboard asynchronously and call insert().
    Result key(xkb_keysym_t sym, const std::string& utf8, uint32_t mods);
};

}  // namespace island
