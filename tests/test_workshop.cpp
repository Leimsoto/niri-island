#include <cstdlib>
#include <unistd.h>

#include "check.hpp"
#include "core/proc.hpp"
#include "core/util.hpp"
#include "services/workshop.hpp"

using namespace island;

TEST(workshop_item) {
    char tmpl[] = "/tmp/niri-island-we-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    workshop::Item it;
    CHECK(!workshop::is_item(dir));
    CHECK(!workshop::read_item(dir, it));

    write_file_atomic(dir + "/preview.gif", "GIF89a");
    write_file_atomic(dir + "/project.json", R"({"title": " Neon city ", "type": "Scene", "preview": "preview.gif"})");
    CHECK(workshop::is_item(dir));
    CHECK(workshop::read_item(dir, it));
    CHECK(it.title == "Neon city");
    CHECK(it.type == "scene");
    CHECK(it.preview == dir + "/preview.gif");

    // no title: the folder's name; a preview outside the folder is ignored
    write_file_atomic(dir + "/project.json", R"({"type": "video", "preview": "../../etc/passwd"})");
    CHECK(workshop::read_item(dir, it));
    CHECK(it.title == dir.substr(dir.rfind('/') + 1));
    CHECK(it.preview.empty());

    write_file_atomic(dir + "/project.json", "not json");
    CHECK(!workshop::read_item(dir, it));

    unlink((dir + "/project.json").c_str());
    unlink((dir + "/preview.gif").c_str());
    rmdir(dir.c_str());
}

#include <algorithm>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>

// Only unsubscribed Workshop downloads go, and only once Steam's list is newer than them.
TEST(workshop_clean_workshop) {
    char tmpl[] = "/tmp/niri-island-steam-XXXXXX";
    std::string home = mkdtemp(tmpl);
    std::string old_home = home_dir();
    setenv("HOME", home.c_str(), 1);
    std::string steam = home + "/.local/share/Steam", ws = steam + "/steamapps/workshop/content/431960";
    make_dirs(ws + "/111");
    make_dirs(ws + "/222");
    make_dirs(ws + "/notanid");
    make_dirs(steam + "/userdata/42/ugc");
    write_file_atomic(steam + "/steamapps/libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam + "\"\n\t}\n}\n");
    std::string vdf = steam + "/userdata/42/ugc/431960_subscriptions.vdf";
    write_file_atomic(vdf, "\"subscribedfiles\"\n{\n\t\"0\"\n\t{\n\t\t\"publishedfileid\"\t\t\"111\"\n\t\t\"disabled_locally\"\t\t\"0\"\n\t}\n}\n");
    auto touch = [&](time_t t) {
        timespec ts[2] = {{t, 0}, {t, 0}};
        utimensat(AT_FDCWD, vdf.c_str(), ts, 0);
    };

    // the list is older than the folders: 222 may be a fresh download, it stays
    touch(time(nullptr) - 3600);
    CHECK(workshop::clean_workshop() == 0);
    CHECK(file_exists(ws + "/222"));

    // Steam rewrote the list since: 222 is unsubscribed, it goes; the rest stays
    touch(time(nullptr) + 3600);
    write_file_atomic(ws + "/222/project.json", "{}");
    CHECK(workshop::clean_workshop() == 1);
    CHECK(!file_exists(ws + "/222"));
    CHECK(file_exists(ws + "/111"));
    CHECK(file_exists(ws + "/notanid"));

    // an empty list deletes nothing
    write_file_atomic(vdf, "\"subscribedfiles\"\n{\n}\n");
    touch(time(nullptr) + 3600);
    CHECK(workshop::clean_workshop() == 0);
    CHECK(file_exists(ws + "/111"));

    setenv("HOME", old_home.c_str(), 1);
    proc::run({"rm", "-rf", home});
}

#include "services/engine.hpp"

// What the picker lists: subscribed downloads, your projects, the bundled ones not hidden.
TEST(workshop_items) {
    char tmpl[] = "/tmp/niri-island-steam-XXXXXX";
    std::string home = mkdtemp(tmpl);
    std::string old_home = home_dir();
    setenv("HOME", home.c_str(), 1);
    std::string steam = home + "/.local/share/Steam", ws = steam + "/steamapps/workshop/content/431960";
    std::string we = steam + "/steamapps/common/wallpaper_engine/projects";
    auto item = [](const std::string& dir, const char* title) {
        make_dirs(dir);
        write_file_atomic(dir + "/project.json", std::string(R"({"type":"scene","title":")") + title + "\"}");
    };
    item(ws + "/111", "subscribed");
    item(ws + "/222", "left behind");
    item(we + "/defaultprojects/beach", "beach");
    item(we + "/defaultprojects/retro", "retro");
    item(we + "/myprojects/mine", "mine");
    write_file_atomic(we + "/defaultprojects/visibility.json", R"({"beach": false})");
    make_dirs(steam + "/userdata/42/ugc");
    write_file_atomic(steam + "/steamapps/libraryfolders.vdf", "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + steam + "\"\n\t}\n}\n");
    std::string vdf = steam + "/userdata/42/ugc/431960_subscriptions.vdf";
    write_file_atomic(vdf, "\"subscribedfiles\"\n{\n\t\"0\"\n\t{\n\t\t\"publishedfileid\"\t\t\"111\"\n\t}\n}\n");
    timespec ts[2] = {{time(nullptr) + 3600, 0}, {time(nullptr) + 3600, 0}};  // Steam rewrote it after the downloads
    utimensat(AT_FDCWD, vdf.c_str(), ts, 0);

    std::vector<std::string> titles;
    for (auto& it : workshop::items()) titles.push_back(it.title);
    auto has = [&](const char* t) { return std::find(titles.begin(), titles.end(), t) != titles.end(); };
    CHECK(has("subscribed"));
    CHECK(!has("left behind"));
    CHECK(has("mine"));
    CHECK(has("retro"));
    CHECK(!has("beach"));  // hidden in Wallpaper Engine

    setenv("HOME", old_home.c_str(), 1);
    proc::run({"rm", "-rf", home});
}

TEST(engine_renders) {
    CHECK(engine::renders("scene"));
    CHECK(engine::renders("video"));
    CHECK(!engine::renders("web"));
    CHECK(!engine::renders("application"));
}
