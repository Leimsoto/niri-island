#include <cmath>

#include "check.hpp"
#include "services/apps.hpp"

using namespace island;

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

TEST(calculator) {
    double v = 0;
    CHECK(calc_eval("2+3*4", v) && near(v, 14));
    CHECK(calc_eval("(2+3)*4", v) && near(v, 20));
    CHECK(calc_eval("2^10", v) && near(v, 1024));
    CHECK(calc_eval("sqrt(16)", v) && near(v, 4));
    CHECK(calc_eval("15% de 80", v) && near(v, 12));
    CHECK(!calc_eval("2+", v));
    CHECK(!calc_eval("hola", v));
}
