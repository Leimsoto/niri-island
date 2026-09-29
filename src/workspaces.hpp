#pragma once

#include <vector>
#include <string>
#include <cstdint>
#include <chrono>

namespace island {

struct WorkspaceInfo {
    uint32_t id = 0;
    uint32_t idx = 1;
    std::string name;
    bool is_active = false;
    bool is_focused = false;
};

class WorkspaceManager {
public:
    WorkspaceManager();
    ~WorkspaceManager();

    bool init();
    void update();
    void focus_workspace(uint32_t idx);

    const std::vector<WorkspaceInfo>& workspaces() const { return workspaces_; }
    uint32_t active_idx() const { return active_idx_; }
    float bubble_width() const;

private:
    std::string socket_path_;
    int socket_fd_ = -1;
    std::vector<WorkspaceInfo> workspaces_;
    uint32_t active_idx_ = 1;
    std::chrono::steady_clock::time_point last_poll_time_;

    void query_workspaces();
    void parse_workspaces_json(const std::string& json_str);
};

} // namespace island
