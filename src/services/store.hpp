#pragma once

#include <cstdint>

#include <sqlite3.h>
#include <string>
#include <vector>

namespace island {

struct Task {
    int id = 0;
    std::string title, notes, date;  // date: YYYY-MM-DD
    int status = 0;                  // 0 to do, 1 doing, 2 done
    int64_t created = 0, completed = 0;
    bool done() const { return status == 2; }
};

struct Note {
    int id = 0;
    std::string title, content;
    int color = 0;
    int64_t created = 0, updated = 0;
    bool archived = false;
};

// tasks.db in the state directory. Same schema as before, so existing tasks
// and notes carry over.
class Store {
public:
    ~Store();
    bool open();

    const std::vector<Task>& tasks() const { return tasks_; }
    const std::vector<Note>& notes() const { return notes_; }
    std::vector<Note> archived_notes() const;
    void unarchive_note(int id);
    Task* task(int id);
    Note* note(int id);

    int add_task(const std::string& title, const std::string& date, int status = 0);
    void set_task_status(int id, int status);
    void set_task_date(int id, const std::string& date);
    void set_task_title(int id, const std::string& title);
    void set_task_notes(int id, const std::string& notes);
    void delete_task(int id);
    void cleanup_done(int64_t older_than_s);

    int add_note(const std::string& title = "", const std::string& content = "", int color = 0);
    void update_note(int id, const std::string& title, const std::string& content);
    void set_note_color(int id, int color);
    void archive_note(int id);
    void delete_note(int id);

    // Days (1..31) of a month that have unfinished tasks.
    std::vector<int> task_days(int year, int m0) const;
    static std::string due_label(const std::string& date);
    static std::string today();

private:
    sqlite3* db_ = nullptr;
    std::vector<Task> tasks_;
    std::vector<Note> notes_;
    void exec(const char* sql);
    void load_tasks();
    void load_notes();
};

}  // namespace island
