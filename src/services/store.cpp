#include "services/store.hpp"

#include <iostream>

#include "core/util.hpp"

namespace island {

namespace {
// RAII prepared statement.
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            std::cerr << "[store] " << sqlite3_errmsg(db) << " in: " << sql << "\n";
            s = nullptr;
        }
    }
    ~Stmt() {
        if (s) sqlite3_finalize(s);
    }
    Stmt& bind(int i, const std::string& v) {
        if (s) sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, int64_t v) {
        if (s) sqlite3_bind_int64(s, i, v);
        return *this;
    }
    Stmt& bind(int i, int v) { return bind(i, static_cast<int64_t>(v)); }
    bool step() { return s && sqlite3_step(s) == SQLITE_ROW; }
    void run() {
        if (s) sqlite3_step(s);
    }
    std::string text(int c) {
        auto* t = reinterpret_cast<const char*>(sqlite3_column_text(s, c));
        return t ? t : "";
    }
    int64_t i64(int c) { return sqlite3_column_int64(s, c); }
};
}  // namespace

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

void Store::exec(const char* sql) {
    if (db_) sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
}

bool Store::open() {
    make_dirs(state_dir());
    std::string path = state_dir() + "/tasks.db";
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::cerr << "[store] cannot open " << path << "\n";
        db_ = nullptr;
        return false;
    }
    sqlite3_busy_timeout(db_, 1500);
    exec("PRAGMA journal_mode=WAL;");
    exec("CREATE TABLE IF NOT EXISTS tasks (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, "
         "date_str TEXT NOT NULL, due_label TEXT, done INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0, "
         "completed_at INTEGER NOT NULL DEFAULT 0, notes TEXT DEFAULT '', status INTEGER DEFAULT 0);");
    // Columns added in later versions; harmless if they exist.
    exec("ALTER TABLE tasks ADD COLUMN notes TEXT DEFAULT '';");
    exec("ALTER TABLE tasks ADD COLUMN status INTEGER DEFAULT 0;");
    exec("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, "
         "content TEXT DEFAULT '', color INTEGER DEFAULT 0, created_at INTEGER NOT NULL DEFAULT 0, "
         "updated_at INTEGER NOT NULL DEFAULT 0, archived INTEGER DEFAULT 0);");
    load_tasks();
    load_notes();
    return true;
}

void Store::load_tasks() {
    tasks_.clear();
    if (!db_) return;
    Stmt q(db_, "SELECT id, title, date_str, done, created_at, completed_at, notes, status FROM tasks "
                "ORDER BY done ASC, date_str ASC, id DESC;");
    while (q.step()) {
        Task t;
        t.id = static_cast<int>(q.i64(0));
        t.title = q.text(1);
        t.date = q.text(2);
        bool done = q.i64(3) != 0;
        t.created = q.i64(4);
        t.completed = q.i64(5);
        t.notes = q.text(6);
        t.status = static_cast<int>(q.i64(7));
        if (done) t.status = 2;
        if (t.status < 0 || t.status > 2) t.status = 0;
        tasks_.push_back(t);
    }
}

void Store::load_notes() {
    notes_.clear();
    if (!db_) return;
    Stmt q(db_, "SELECT id, title, content, color, created_at, updated_at FROM notes WHERE archived = 0 "
                "ORDER BY updated_at DESC, id DESC;");
    while (q.step()) {
        Note n;
        n.id = static_cast<int>(q.i64(0));
        n.title = q.text(1);
        n.content = q.text(2);
        n.color = std::clamp(static_cast<int>(q.i64(3)), 0, 5);
        n.created = q.i64(4);
        n.updated = q.i64(5);
        notes_.push_back(n);
    }
}

Task* Store::task(int id) {
    for (auto& t : tasks_)
        if (t.id == id) return &t;
    return nullptr;
}

Note* Store::note(int id) {
    for (auto& n : notes_)
        if (n.id == id) return &n;
    return nullptr;
}

int Store::add_task(const std::string& title, const std::string& date, int status) {
    if (!db_ || trim(title).empty()) return -1;
    Stmt q(db_, "INSERT INTO tasks (title, date_str, due_label, done, created_at, completed_at, notes, status) "
                "VALUES (?, ?, '', ?, ?, ?, '', ?);");
    int64_t now = now_epoch();
    q.bind(1, trim(title)).bind(2, date).bind(3, status == 2 ? 1 : 0).bind(4, now).bind(5, status == 2 ? now : 0)
        .bind(6, status);
    q.run();
    int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    load_tasks();
    return id;
}

void Store::set_task_status(int id, int status) {
    if (!db_) return;
    status = std::clamp(status, 0, 2);
    Stmt q(db_, "UPDATE tasks SET status = ?, done = ?, completed_at = ? WHERE id = ?;");
    q.bind(1, status).bind(2, status == 2 ? 1 : 0).bind(3, status == 2 ? now_epoch() : 0).bind(4, id);
    q.run();
    load_tasks();
}

void Store::set_task_date(int id, const std::string& date) {
    if (!db_) return;
    Stmt q(db_, "UPDATE tasks SET date_str = ? WHERE id = ?;");
    q.bind(1, date).bind(2, id);
    q.run();
    load_tasks();
}

void Store::set_task_title(int id, const std::string& title) {
    if (!db_ || trim(title).empty()) return;
    Stmt q(db_, "UPDATE tasks SET title = ? WHERE id = ?;");
    q.bind(1, trim(title)).bind(2, id);
    q.run();
    load_tasks();
}

void Store::set_task_notes(int id, const std::string& notes) {
    if (!db_) return;
    Stmt q(db_, "UPDATE tasks SET notes = ? WHERE id = ?;");
    q.bind(1, notes).bind(2, id);
    q.run();
    if (Task* t = task(id)) t->notes = notes;
}

void Store::delete_task(int id) {
    if (!db_) return;
    Stmt q(db_, "DELETE FROM tasks WHERE id = ?;");
    q.bind(1, id);
    q.run();
    load_tasks();
}

void Store::cleanup_done(int64_t older_than_s) {
    if (!db_) return;
    Stmt q(db_, "DELETE FROM tasks WHERE done = 1 AND completed_at > 0 AND completed_at < ?;");
    q.bind(1, now_epoch() - older_than_s);
    q.run();
    if (sqlite3_changes(db_) > 0) load_tasks();
}

int Store::add_note(const std::string& title, const std::string& content, int color) {
    if (!db_) return -1;
    Stmt q(db_, "INSERT INTO notes (title, content, color, created_at, updated_at, archived) VALUES (?, ?, ?, ?, ?, 0);");
    int64_t now = now_epoch();
    q.bind(1, title).bind(2, content).bind(3, color).bind(4, now).bind(5, now);
    q.run();
    int id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    load_notes();
    return id;
}

void Store::update_note(int id, const std::string& title, const std::string& content) {
    if (!db_) return;
    Stmt q(db_, "UPDATE notes SET title = ?, content = ?, updated_at = ? WHERE id = ?;");
    int64_t now = now_epoch();
    q.bind(1, title).bind(2, content).bind(3, now).bind(4, id);
    q.run();
    if (Note* n = note(id)) {
        n->title = title;
        n->content = content;
        n->updated = now;
    }
}

void Store::set_note_color(int id, int color) {
    if (!db_) return;
    Stmt q(db_, "UPDATE notes SET color = ?, updated_at = ? WHERE id = ?;");
    q.bind(1, color).bind(2, now_epoch()).bind(3, id);
    q.run();
    if (Note* n = note(id)) n->color = color;
}

void Store::archive_note(int id) {
    if (!db_) return;
    Stmt q(db_, "UPDATE notes SET archived = 1 WHERE id = ?;");
    q.bind(1, id);
    q.run();
    load_notes();
}

std::vector<Note> Store::archived_notes() const {
    std::vector<Note> out;
    if (!db_) return out;
    Stmt q(db_, "SELECT id, title, content, color, created_at, updated_at FROM notes WHERE archived = 1 ORDER BY updated_at DESC;");
    while (q.step()) {
        Note n;
        n.id = static_cast<int>(q.i64(0));
        n.title = q.text(1);
        n.content = q.text(2);
        n.color = std::clamp(static_cast<int>(q.i64(3)), 0, 5);
        n.created = q.i64(4);
        n.updated = q.i64(5);
        n.archived = true;
        out.push_back(n);
    }
    return out;
}

void Store::unarchive_note(int id) {
    if (!db_) return;
    Stmt q(db_, "UPDATE notes SET archived = 0 WHERE id = ?;");
    q.bind(1, id);
    q.run();
    load_notes();
}

void Store::delete_note(int id) {
    if (!db_) return;
    Stmt q(db_, "DELETE FROM notes WHERE id = ?;");
    q.bind(1, id);
    q.run();
    load_notes();
}

std::vector<int> Store::task_days(int year, int m0) const {
    std::vector<int> days;
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%04d-%02d-", year % 10000, (m0 % 12) + 1);
    for (auto& t : tasks_) {
        if (t.done() || !starts_with(t.date, prefix)) continue;
        int d = std::atoi(t.date.c_str() + 8);
        if (d > 0 && std::find(days.begin(), days.end(), d) == days.end()) days.push_back(d);
    }
    return days;
}

std::string Store::today() { return fmt_time("%Y-%m-%d"); }

std::string Store::due_label(const std::string& date) {
    std::string td = today();
    std::string tm = fmt_time("%Y-%m-%d", time(nullptr) + 86400);
    std::string yd = fmt_time("%Y-%m-%d", time(nullptr) - 86400);
    if (date == td) return "hoy";
    if (date == tm) return "mañana";
    if (date == yd) return "ayer";
    static const char* months[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};
    int y = 0, m = 0, d = 0;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
        std::string s = std::to_string(d) + " " + months[m - 1];
        return date < td ? "vencida · " + s : s;
    }
    return date;
}

}  // namespace island
