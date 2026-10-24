#include <cstdlib>
#include <unistd.h>

#include "check.hpp"
#include "core/util.hpp"
#include "services/waywallen.hpp"

using namespace island;

TEST(waywallen_item) {
    char tmpl[] = "/tmp/niri-island-we-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    waywallen::Item it;
    CHECK(!waywallen::is_item(dir));
    CHECK(!waywallen::read_item(dir, it));

    write_file_atomic(dir + "/preview.gif", "GIF89a");
    write_file_atomic(dir + "/project.json", R"({"title": " Neon city ", "type": "Scene", "preview": "preview.gif"})");
    CHECK(waywallen::is_item(dir));
    CHECK(waywallen::read_item(dir, it));
    CHECK(it.title == "Neon city");
    CHECK(it.type == "scene");
    CHECK(it.preview == dir + "/preview.gif");

    // no title: the folder's name; a preview outside the folder is ignored
    write_file_atomic(dir + "/project.json", R"({"type": "video", "preview": "../../etc/passwd"})");
    CHECK(waywallen::read_item(dir, it));
    CHECK(it.title == dir.substr(dir.rfind('/') + 1));
    CHECK(it.preview.empty());

    write_file_atomic(dir + "/project.json", "not json");
    CHECK(!waywallen::read_item(dir, it));

    unlink((dir + "/project.json").c_str());
    unlink((dir + "/preview.gif").c_str());
    rmdir(dir.c_str());
}
