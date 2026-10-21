#include "check.hpp"
#include "core/util.hpp"

using namespace island;

TEST(utf8_length_and_steps) {
    std::string s = "añ€😀";  // 1, 2, 3 and 4 bytes
    CHECK_EQ(utf8_length(s), size_t(4));
    CHECK_EQ(utf8_prev(s, s.size()), s.size() - 4);
    CHECK_EQ(utf8_next(s, 0), size_t(1));
    CHECK_EQ(utf8_next(s, 1), size_t(3));
}

TEST(strings) {
    CHECK_EQ(lower("ÁbC"), std::string("Ábc"));  // ASCII only, UTF-8 left alone
    CHECK_EQ(trim("  hola \n"), std::string("hola"));
    CHECK_EQ(replace_all("a-b-c", "-", "+"), std::string("a+b+c"));
    auto parts = split("a,b,,c", ',');
    CHECK_EQ(parts.size(), size_t(4));
    CHECK(starts_with("niri-island", "niri"));
    CHECK(ends_with("foto.png", ".png"));
    CHECK(!starts_with("ni", "niri"));
}

TEST(markup_is_escaped) {
    CHECK_EQ(escape_markup("<b>&\""), std::string("&lt;b&gt;&amp;&quot;"));
    CHECK_EQ(strip_markup("<b>hola</b> mundo"), std::string("hola mundo"));
}

TEST(shell_quote_is_safe) {
    CHECK_EQ(shell_quote("a b"), std::string("'a b'"));
    CHECK_EQ(shell_quote("it's"), std::string("'it'\\''s'"));
}

TEST(durations_and_calendar) {
    CHECK_EQ(fmt_duration(65), std::string("1:05"));
    CHECK_EQ(fmt_duration(3725), std::string("1:02:05"));
    CHECK_EQ(days_in_month(2024, 1), 29);  // leap February
    CHECK_EQ(days_in_month(2026, 1), 28);
    CHECK_EQ(date_iso(2026, 9, 21), std::string("2026-10-21"));
}
