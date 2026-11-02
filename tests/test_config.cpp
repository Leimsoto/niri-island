#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <unistd.h>
#include <cairo.h>

#include "check.hpp"
#include "core/config.hpp"
#include "core/util.hpp"
#include "ui/paint.hpp"

using namespace island;

// A config file in a throw-away XDG_CONFIG_HOME.
static Config load_from(const std::string& text) {
    char tmpl[] = "/tmp/niri-island-test-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    make_dirs(dir + "/niri-island");
    write_file_atomic(dir + "/niri-island/config.json", text);
    Config c;
    c.load();
    unlink((dir + "/niri-island/config.json").c_str());
    rmdir((dir + "/niri-island").c_str());
    rmdir(dir.c_str());
    return c;
}

static const ModuleConf* mod(const Config& c, const std::string& id) {
    for (auto& m : c.modules)
        if (m.id == id) return &m;
    return nullptr;
}

TEST(config_defaults_for_missing_keys) {
    Config c = load_from("{}");
    CHECK_EQ(c.theme, std::string("adaptive"));
    CHECK(mod(c, "perf") && !mod(c, "perf")->on);  // the performance island starts off
    CHECK(mod(c, "battery") && mod(c, "battery")->on);
    CHECK(mod(c, "apps") && mod(c, "apps")->on);
    CHECK_EQ(mod(c, "apps")->side, std::string("left"));
    CHECK(c.apps_split);
    CHECK_EQ(c.apps_limit, 5);
}

TEST(config_new_modules_are_merged) {
    // an older file without "perf" and "tray": they are added, the rest kept
    Config c = load_from(R"({"appearance":{"modules":[{"id":"battery","side":"left","on":false}]}})");
    CHECK(mod(c, "battery") && mod(c, "battery")->side == "left" && !mod(c, "battery")->on);
    CHECK(mod(c, "perf") != nullptr);
    CHECK(mod(c, "tray") != nullptr);
    CHECK(mod(c, "apps") != nullptr);
}

TEST(config_round_trip) {
    Config a;
    a.perf_metrics = {"cpu", "net"};
    a.perf_style = "rings";
    a.perf_split = true;
    a.apps_split = false;
    a.apps_limit = 8;
    a.wallpaper_picker_style = "hex";
    a.battery_percent = false;
    Config b = load_from(a.to_json());
    CHECK(b.perf_metrics == a.perf_metrics);
    CHECK_EQ(b.perf_style, std::string("rings"));
    CHECK(b.perf_split);
    CHECK(!b.apps_split);
    CHECK_EQ(b.apps_limit, 8);
    CHECK_EQ(b.wallpaper_picker_style, std::string("hex"));
    CHECK(!b.battery_percent);
}

TEST(config_bad_json_keeps_defaults) {
    Config c = load_from("{ esto no es json");
    CHECK_EQ(c.theme, std::string("adaptive"));
}

TEST(image_mono_grayscale) {
    cairo_surface_t* src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_surface_flush(src);
    uint32_t* sdata = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(src));
    sdata[0] = 0xffff0000;  // Rojo opaco
    sdata[1] = 0xff00ff00;  // Verde opaco
    sdata[2] = 0xff0000ff;  // Azul opaco
    sdata[3] = 0xffe08020;  // Color mixto
    cairo_surface_mark_dirty(src);

    cairo_surface_t* dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2, 2);
    cairo_t* cr = cairo_create(dst);
    paint::image_mono(cr, src, {0, 0, 2, 2}, 0, Color{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f);
    cairo_destroy(cr);

    cairo_surface_flush(dst);
    uint32_t* ddata = reinterpret_cast<uint32_t*>(cairo_image_surface_get_data(dst));
    for (int i = 0; i < 4; ++i) {
        uint32_t px = ddata[i];
        uint32_t a = (px >> 24) & 0xff;
        uint32_t r = (px >> 16) & 0xff;
        uint32_t g = (px >> 8) & 0xff;
        uint32_t b = px & 0xff;
        CHECK_EQ(a, 0xffu);
        CHECK_EQ(r, g);
        CHECK_EQ(g, b);
        CHECK(r > 0);
    }

    cairo_surface_destroy(dst);
    cairo_surface_destroy(src);
}
