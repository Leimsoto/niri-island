#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace island {

// inotify on the parent directory, so files replaced atomically (write to a
// temp file + rename, as most tools do) keep being noticed.
class FileWatch {
public:
    ~FileWatch();
    void watch(const std::string& path, std::function<void()> cb);

private:
    int fd_ = -1;
    struct Entry {
        std::string name;
        std::function<void()> cb;
        uint64_t debounce = 0;
    };
    std::map<int, std::string> dirs_;          // wd -> dir
    std::multimap<std::string, Entry*> files_; // dir -> entries
    std::vector<Entry*> owned_;
    void readable();
};

}  // namespace island
