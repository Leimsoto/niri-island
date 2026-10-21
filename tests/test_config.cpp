#include <algorithm>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "check.hpp"
#include "core/config.hpp"
#include "core/util.hpp"

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
}

TEST(config_new_modules_are_merged) {
    // an older file without "perf" and "tray": they are added, the rest kept
    Config c = load_from(R"({"appearance":{"modules":[{"id":"battery","side":"left","on":false}]}})");
    CHECK(mod(c, "battery") && mod(c, "battery")->side == "left" && !mod(c, "battery")->on);
    CHECK(mod(c, "perf") != nullptr);
    CHECK(mod(c, "tray") != nullptr);
}

TEST(config_round_trip) {
    Config a;
    a.perf_metrics = {"cpu", "net"};
    a.perf_style = "rings";
    a.perf_split = true;
    a.wallpaper_picker_style = "hex";
    a.battery_percent = false;
    Config b = load_from(a.to_json());
    CHECK(b.perf_metrics == a.perf_metrics);
    CHECK_EQ(b.perf_style, std::string("rings"));
    CHECK(b.perf_split);
    CHECK_EQ(b.wallpaper_picker_style, std::string("hex"));
    CHECK(!b.battery_percent);
}

TEST(config_bad_json_keeps_defaults) {
    Config c = load_from("{ esto no es json");
    CHECK_EQ(c.theme, std::string("adaptive"));
}
