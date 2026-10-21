#pragma once

// A tiny test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
// The runner prints each failure and returns how many tests failed.

#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Test>& all() {
    static std::vector<Test> t;
    return t;
}
inline int& failures() {
    static int f = 0;
    return f;
}
struct Add {
    Add(const char* n, std::function<void()> f) { all().push_back({n, std::move(f)}); }
};
inline void fail(const char* file, int line, const std::string& what) {
    ++failures();
    std::cerr << "  FALLO " << file << ":" << line << ": " << what << "\n";
}

}  // namespace check

#define TEST(name)                                         \
    static void test_##name();                             \
    static check::Add add_##name(#name, test_##name);      \
    static void test_##name()

#define CHECK(cond) \
    do {            \
        if (!(cond)) check::fail(__FILE__, __LINE__, #cond); \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        auto va_ = (a);                                                                  \
        auto vb_ = (b);                                                                  \
        if (!(va_ == vb_)) {                                                             \
            std::ostringstream os_;                                                      \
            os_ << #a << " == " << #b << "  (" << va_ << " vs " << vb_ << ")";           \
            check::fail(__FILE__, __LINE__, os_.str());                                  \
        }                                                                                \
    } while (0)
