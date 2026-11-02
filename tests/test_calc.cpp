#include <cmath>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

#include "check.hpp"
#include "core/loop.hpp"
#include "core/util.hpp"
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

static void save_env(const char* name, bool& had, std::string& val) {
    const char* v = getenv(name);
    had = (v != nullptr);
    val = v ? v : "";
}

static void restore_env(const char* name, bool had, const std::string& val) {
    if (had) setenv(name, val.c_str(), 1);
    else unsetenv(name);
}

TEST(apps_search_all) {
    // 1) System apps scan: verify search("") returns all indexed apps without 24-item truncation.
    {
        Loop loop;
        Apps apps;
        apps.on_change = [&] { loop.quit(); };
        loop.after(5.0, [&] { loop.quit(); });
        apps.rescan();
        loop.run();

        CHECK(apps.ready());
        std::cout << "  [apps] aplicaciones del sistema indexadas: " << apps.all().size() << "\n";
        CHECK_EQ(apps.search("").size(), apps.all().size());
        if (apps.all().size() > 24) {
            CHECK_EQ(apps.search("", 24).size(), static_cast<size_t>(24));
            CHECK(apps.search("").size() > 24);
        }
    }

    // 2) Temporary XDG_DATA_HOME / XDG_DATA_DIRS with 35 synthetic .desktop entries (> 30).
    bool had_dh = false, had_dd = false;
    std::string old_dh, old_dd;
    save_env("XDG_DATA_HOME", had_dh, old_dh);
    save_env("XDG_DATA_DIRS", had_dd, old_dd);

    char tmpl[] = "/tmp/niri-island-apps-XXXXXX";
    std::string base = mkdtemp(tmpl);
    std::string dh_dir = base + "/home";
    std::string dd_dir = base + "/share";
    make_dirs(dh_dir + "/applications");
    make_dirs(dd_dir + "/applications");

    constexpr int kCount = 35;
    for (int i = 0; i < kCount; ++i) {
        std::string dir = (i < 15) ? (dh_dir + "/applications") : (dd_dir + "/applications");
        std::string path = dir + "/zqx-synth-app-" + std::to_string(i) + ".desktop";
        std::string body = "[Desktop Entry]\n"
                           "Type=Application\n"
                           "Name=ZqxSynthApp " + std::to_string(i) + "\n"
                           "GenericName=Synthetic Tool " + std::to_string(i) + "\n"
                           "Exec=/bin/true %U\n"
                           "Icon=application-x-executable\n";
        write_file_atomic(path, body);
    }

    setenv("XDG_DATA_HOME", dh_dir.c_str(), 1);
    setenv("XDG_DATA_DIRS", dd_dir.c_str(), 1);

    {
        Loop loop;
        Apps apps;
        apps.on_change = [&] { loop.quit(); };
        loop.after(5.0, [&] { loop.quit(); });
        apps.rescan();
        loop.run();

        CHECK(apps.ready());
        CHECK_EQ(apps.search("").size(), apps.all().size());
        CHECK(apps.search("").size() >= static_cast<size_t>(kCount));
        CHECK_EQ(apps.search("ZqxSynthApp").size(), static_cast<size_t>(kCount));
        auto specific = apps.search("ZqxSynthApp 29");
        CHECK(!specific.empty());
        CHECK_EQ(specific.front().app->name, std::string("ZqxSynthApp 29"));
    }

    restore_env("XDG_DATA_HOME", had_dh, old_dh);
    restore_env("XDG_DATA_DIRS", had_dd, old_dd);

    for (int i = 0; i < kCount; ++i) {
        std::string dir = (i < 15) ? (dh_dir + "/applications") : (dd_dir + "/applications");
        std::string path = dir + "/zqx-synth-app-" + std::to_string(i) + ".desktop";
        unlink(path.c_str());
    }
    rmdir((dh_dir + "/applications").c_str());
    rmdir(dh_dir.c_str());
    rmdir((dd_dir + "/applications").c_str());
    rmdir(dd_dir.c_str());
    rmdir(base.c_str());
}
