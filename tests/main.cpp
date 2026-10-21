// niri-island's tests: run `ctest` in the build directory (or the binary).

#include <clocale>

#include "check.hpp"

int main() {
    setlocale(LC_NUMERIC, "C");
    int failed_tests = 0;
    for (auto& t : check::all()) {
        int before = check::failures();
        t.fn();
        bool ok = check::failures() == before;
        if (!ok) ++failed_tests;
        std::cout << (ok ? "  ok    " : "  FALLA ") << t.name << "\n";
    }
    std::cout << check::all().size() - failed_tests << "/" << check::all().size() << " tests correctos\n";
    return failed_tests ? 1 : 0;
}
