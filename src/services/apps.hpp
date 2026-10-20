#pragma once

#include <cstdint>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace island {

struct AppEntry {
    std::string id;        // desktop file id, "org.gnome.Nautilus"
    std::string name, generic, comment, exec, icon, keywords;
    bool terminal = false;
};

class Apps {
public:
    void init();
    bool ready() const { return ready_; }
    const std::vector<AppEntry>& all() const { return apps_; }

    struct Match {
        const AppEntry* app;
        int score;
    };
    std::vector<Match> search(const std::string& query, size_t limit) const;
    void launch(const AppEntry& app);
    int uses(const std::string& id) const;

    std::function<void()> on_change;

private:
    std::vector<AppEntry> apps_;
    std::map<std::string, int> usage_;
    bool ready_ = false;
    void load_usage();
    void save_usage();
};

// "2+3*4", "sqrt(2)", "2^10", "15% of 80" → result; false when not maths.
bool calc_eval(const std::string& expr, double& out);
std::string calc_format(double v);

}  // namespace island
