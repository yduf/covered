#include "db.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace covered {

static int exec_sql(sqlite3* db, const char* sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg) {
            sqlite3_free(errmsg);
        }
    }
    return rc;
}

Database::Database(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    // Store the DB folder path (strip "/filesize.db" suffix)
    db_folder_ = std::filesystem::path(path).parent_path().string();

    // Speed / reliability tuning
    exec_sql(db_, "PRAGMA journal_mode=WAL;");
    exec_sql(db_, "PRAGMA synchronous=NORMAL;");
    exec_sql(db_, "PRAGMA cache_size=-64000;");   // 64 MB
    exec_sql(db_, "PRAGMA temp_store=MEMORY;");
    exec_sql(db_, "PRAGMA mmap_size=30000000000;");

    // Schema (CREATE TABLE IF NOT EXISTS — safe for new DBs)
    // Note: meta table has been removed (info stored in config.json instead).
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS dirs (
            inode        INTEGER NOT NULL,
            parent_inode INTEGER,
            name         TEXT    NOT NULL,
            error        INTEGER DEFAULT NULL,
            delta        INTEGER DEFAULT NULL,
            PRIMARY KEY (inode)
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS files (
            dir_inode  INTEGER NOT NULL,
            name       TEXT    NOT NULL,
            inode      INTEGER NOT NULL,
            size       INTEGER NOT NULL,
            mtime      INTEGER NOT NULL,
            covered    INTEGER NOT NULL DEFAULT 0,
            error      INTEGER DEFAULT NULL,
            backup_id  INTEGER DEFAULT NULL,
            delta      INTEGER DEFAULT NULL,
            PRIMARY KEY (dir_inode, name)
        ) WITHOUT ROWID;

        CREATE INDEX IF NOT EXISTS idx_files_inode ON files(inode);
        CREATE INDEX IF NOT EXISTS idx_files_size  ON files(size);

        CREATE TABLE IF NOT EXISTS backup_db (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            path TEXT NOT NULL UNIQUE
        );
    )";

    rc = exec_sql(db_, schema);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    // Backward compat: add error columns to existing DBs that lack them.
    // Must run BEFORE prepared statements that reference the column.
    migrate_error_columns();
    migrate_backup_id_column();
    migrate_backup_db_table();
    migrate_drop_meta_table();
    migrate_delta_columns();

    // Load backup paths into cache
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, path FROM backup_db";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (p) backup_paths_[id] = p;
            }
            sqlite3_finalize(stmt);
        }
    }

    // Prepared statements for bulk insert
    const char* sql_dir = "INSERT INTO dirs (inode, parent_inode, name, error, delta) VALUES (?, ?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(db_, sql_dir, -1, &stmt_dir_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    const char* sql_file = "INSERT INTO files (dir_inode, name, inode, size, mtime, covered, error, backup_id, delta) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(db_, sql_file, -1, &stmt_file_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }
}

Database::~Database() {
    if (stmt_dir_)  sqlite3_finalize(stmt_dir_);
    if (stmt_file_) sqlite3_finalize(stmt_file_);
    if (db_)        sqlite3_close(db_);
}

std::optional<std::string> Database::read_config_json() const {
    std::string config_path = db_folder_ + "/config.json";
    std::ifstream f(config_path);
    if (!f) return std::nullopt;
    try {
        nlohmann::json config = nlohmann::json::parse(f);
        if (config.contains("root") && config["root"].is_string()) {
            return config["root"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // fall through
    }
    return std::nullopt;
}

std::optional<std::string> Database::get_root_path() {
    std::lock_guard<std::mutex> lock(mutex_);
    return read_config_json();
}

uint64_t Database::count_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM files";
    uint64_t count = 0;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

std::vector<int64_t> Database::get_distinct_sizes() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int64_t> sizes;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT DISTINCT size FROM files WHERE size > 0 ORDER BY size";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sizes.push_back(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return sizes;
}

std::vector<FileEntry> Database::get_files_by_size(int64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileEntry> files;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT dir_inode, name, inode, size, mtime, covered, error FROM files WHERE size = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, size);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            files.push_back({
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)),
                sqlite3_column_int64(stmt, 3),
                sqlite3_column_int64(stmt, 4),
                sqlite3_column_int(stmt, 5),
                sqlite3_column_int(stmt, 6)
            });
        }
        sqlite3_finalize(stmt);
    }
    return files;
}

void Database::set_covered(uint64_t inode, int covered) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET covered = ? WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, covered);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::set_file_error(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET error = 1 WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// ------------------------------------------------------------------
// Report-phase helpers
// ------------------------------------------------------------------

void Database::migrate_dirs_covered_column() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Ignore error – column may already exist
    exec_sql(db_, "ALTER TABLE dirs ADD COLUMN covered INTEGER NOT NULL DEFAULT 0;");
}

void Database::migrate_error_columns() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Ignore error – columns may already exist (new DBs have them from the CREATE TABLE;
    // existing DBs get them via ALTER with DEFAULT NULL)
    exec_sql(db_, "ALTER TABLE dirs ADD COLUMN error INTEGER DEFAULT NULL;");
    exec_sql(db_, "ALTER TABLE files ADD COLUMN error INTEGER DEFAULT NULL;");
}

void Database::migrate_backup_id_column() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Ignore error – column may already exist
    exec_sql(db_, "ALTER TABLE files ADD COLUMN backup_id INTEGER DEFAULT NULL;");
}

void Database::migrate_backup_db_table() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Ignore error – table may already exist
    exec_sql(db_, "CREATE TABLE IF NOT EXISTS backup_db ("
                   "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
                   "  path TEXT NOT NULL UNIQUE"
                   ");");
}

void Database::migrate_delta_columns() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Ignore error – columns may already exist
    exec_sql(db_, "ALTER TABLE dirs ADD COLUMN delta INTEGER DEFAULT NULL;");
    exec_sql(db_, "ALTER TABLE files ADD COLUMN delta INTEGER DEFAULT NULL;");
}

void Database::migrate_drop_meta_table() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Safe to drop — meta data is now stored in config.json.
    // Before dropping, migrate root_path to config.json if not already present.
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT value FROM meta WHERE key = 'root_path'";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (text) {
                    std::string root_path(text);
                    // Check if config.json already has root
                    auto existing = read_config_json();
                    if (!existing.has_value()) {
                        // Write to config.json
                        std::string config_path = db_folder_ + "/config.json";
                        nlohmann::json config;
                        config["root"] = root_path;
                        config["device"] = 0; // device unknown for old DBs
                        std::ofstream cfg(config_path);
                        if (cfg) {
                            cfg << config.dump() << "\n";
                        }
                    }
                }
            }
            sqlite3_finalize(stmt);
        }
    }
    exec_sql(db_, "DROP TABLE IF EXISTS meta;");
}

int Database::register_backup_db(const std::string& backup_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // First check if this backup path is already registered
    sqlite3_stmt* stmt = nullptr;
    const char* sql_check = "SELECT id FROM backup_db WHERE path = ?";
    if (sqlite3_prepare_v2(db_, sql_check, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, backup_path.c_str(), static_cast<int>(backup_path.size()), SQLITE_STATIC);
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            return id; // already registered, return existing id
        }
        sqlite3_finalize(stmt);
    }

    // Not registered yet, insert it
    const char* sql_insert = "INSERT INTO backup_db (path) VALUES (?)";
    if (sqlite3_prepare_v2(db_, sql_insert, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, backup_path.c_str(), static_cast<int>(backup_path.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Return the new id (last_insert_rowid)
    return static_cast<int>(sqlite3_last_insert_rowid(db_));
}

std::string Database::get_backup_path(int backup_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT path FROM backup_db WHERE id = ?";
    std::string result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, backup_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (text) result = std::string(text);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

void Database::set_file_backup_id(uint64_t inode, int backup_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET backup_id = ? WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, backup_id);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::vector<DirEntry> Database::get_all_dirs() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DirEntry> dirs;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT inode, parent_inode, name, covered, error, delta FROM dirs";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            DirEntry d;
            d.inode        = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            // parent_inode may be NULL for root
            if (sqlite3_column_type(stmt, 1) == SQLITE_NULL)
                d.parent_inode = 0;
            else
                d.parent_inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            d.name    = name ? name : "";
            d.covered = sqlite3_column_int(stmt, 3);
            d.error   = sqlite3_column_int(stmt, 4);
            d.delta   = sqlite3_column_type(stmt, 5) == SQLITE_NULL ? 0 : sqlite3_column_int(stmt, 5);
            dirs.push_back(d);
        }
        sqlite3_finalize(stmt);
    }
    return dirs;
}

std::vector<FileEntry> Database::get_files_by_dir(uint64_t dir_inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileEntry> files;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT dir_inode, name, inode, size, mtime, covered, error FROM files WHERE dir_inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(dir_inode));
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            files.push_back({
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)),
                sqlite3_column_int64(stmt, 3),
                sqlite3_column_int64(stmt, 4),
                sqlite3_column_int(stmt, 5),
                sqlite3_column_int(stmt, 6)
            });
        }
        sqlite3_finalize(stmt);
    }
    return files;
}

std::vector<FileEntry> Database::get_all_files() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileEntry> files;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT dir_inode, name, inode, size, mtime, covered, error, backup_id, delta FROM files";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int bid = 0;
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
                bid = sqlite3_column_int(stmt, 7);
            int d = 0;
            if (sqlite3_column_type(stmt, 8) != SQLITE_NULL)
                d = sqlite3_column_int(stmt, 8);
            files.push_back({
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)),
                sqlite3_column_int64(stmt, 3),
                sqlite3_column_int64(stmt, 4),
                sqlite3_column_int(stmt, 5),
                sqlite3_column_int(stmt, 6),
                bid,
                d
            });
        }
        sqlite3_finalize(stmt);
    }
    return files;
}

void Database::set_dir_covered(uint64_t inode, int covered) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE dirs SET covered = ? WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, covered);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::begin_batch() {
    std::lock_guard<std::mutex> lock(mutex_);
    exec_sql(db_, "BEGIN IMMEDIATE;");
}

void Database::add_dir(const DirEntry& d) {
    std::lock_guard<std::mutex> lock(mutex_);
    dir_buffer_.push_back(d);
    if (dir_buffer_.size() >= BATCH_SIZE) {
        flush_dirs();
    }
}

void Database::add_file(const FileEntry& f) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_buffer_.push_back(f);
    if (file_buffer_.size() >= BATCH_SIZE) {
        flush_files();
    }
}

void Database::commit_batch() {
    std::lock_guard<std::mutex> lock(mutex_);
    flush_dirs();
    flush_files();
    exec_sql(db_, "COMMIT;");
}

void Database::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    }
}

void Database::flush_dirs() {
    if (!db_ || !stmt_dir_) return;
    for (const auto& d : dir_buffer_) {
        sqlite3_reset(stmt_dir_);
        sqlite3_bind_int64(stmt_dir_, 1, static_cast<sqlite3_int64>(d.inode));
        if (d.parent_inode == 0)
            sqlite3_bind_null(stmt_dir_, 2);
        else
            sqlite3_bind_int64(stmt_dir_, 2, static_cast<sqlite3_int64>(d.parent_inode));
        sqlite3_bind_text(stmt_dir_, 3, d.name.c_str(), static_cast<int>(d.name.size()), SQLITE_STATIC);
        if (d.error)
            sqlite3_bind_int(stmt_dir_, 4, d.error);
        else
            sqlite3_bind_null(stmt_dir_, 4);
        if (d.delta != 0)
            sqlite3_bind_int(stmt_dir_, 5, d.delta);
        else
            sqlite3_bind_null(stmt_dir_, 5);
        int rc = sqlite3_step(stmt_dir_);
        if (rc != SQLITE_DONE) {
            error_ = true;
            error_msg_ = sqlite3_errmsg(db_);
            break;
        }
    }
    dir_buffer_.clear();
}

void Database::flush_files() {
    if (!db_ || !stmt_file_) return;
    for (const auto& f : file_buffer_) {
        sqlite3_reset(stmt_file_);
        sqlite3_bind_int64(stmt_file_, 1, static_cast<sqlite3_int64>(f.dir_inode));
        sqlite3_bind_text(stmt_file_, 2, f.name.c_str(), static_cast<int>(f.name.size()), SQLITE_STATIC);
        sqlite3_bind_int64(stmt_file_, 3, static_cast<sqlite3_int64>(f.inode));
        sqlite3_bind_int64(stmt_file_, 4, static_cast<sqlite3_int64>(f.size));
        sqlite3_bind_int64(stmt_file_, 5, static_cast<sqlite3_int64>(f.mtime));
        sqlite3_bind_int(stmt_file_, 6, f.covered);
        if (f.error)
            sqlite3_bind_int(stmt_file_, 7, f.error);
        else
            sqlite3_bind_null(stmt_file_, 7);
        if (f.backup_id > 0)
            sqlite3_bind_int(stmt_file_, 8, f.backup_id);
        else
            sqlite3_bind_null(stmt_file_, 8);
        if (f.delta != 0)
            sqlite3_bind_int(stmt_file_, 9, f.delta);
        else
            sqlite3_bind_null(stmt_file_, 9);
        int rc = sqlite3_step(stmt_file_);
        if (rc != SQLITE_DONE) {
            error_ = true;
            error_msg_ = sqlite3_errmsg(db_);
            break;
        }
    }
    file_buffer_.clear();
}

// ------------------------------------------------------------------
// Shared algorithms: compute_dir_covered + build_dir_paths
// ------------------------------------------------------------------

std::unordered_map<uint64_t, std::string>
Database::build_dir_paths(const std::string& root_path) {
    auto dirs = get_all_dirs();

    std::unordered_map<uint64_t, const DirEntry*> by_inode;
    for (const auto& d : dirs) {
        by_inode[d.inode] = &d;
    }

    std::unordered_map<uint64_t, std::string> paths;
    std::function<std::string(uint64_t)> get_path = [&](uint64_t inode) -> std::string {
        auto it = paths.find(inode);
        if (it != paths.end()) return it->second;

        auto dit = by_inode.find(inode);
        if (dit == by_inode.end()) return root_path;

        const auto* d = dit->second;
        if (d->parent_inode == 0) {
            paths[inode] = root_path;
        } else {
            paths[inode] = get_path(d->parent_inode) + "/" + d->name;
        }
        return paths[inode];
    };

    for (const auto& d : dirs) {
        get_path(d.inode);
    }
    return paths;
}

std::unordered_map<uint64_t, int>
Database::compute_dir_covered() {
    auto dirs = get_all_dirs();

    // Build children map
    std::unordered_map<uint64_t, std::vector<uint64_t>> children;
    uint64_t root_inode = 0;
    for (const auto& d : dirs) {
        if (d.parent_inode == 0) {
            root_inode = d.inode;
        } else {
            children[d.parent_inode].push_back(d.inode);
        }
    }

    std::unordered_map<uint64_t, int> result;

    // Post-order DFS (process children before parents)
    std::vector<uint64_t> order;
    {
        std::vector<uint64_t> stack;
        if (root_inode != 0) stack.push_back(root_inode);
        for (const auto& d : dirs) {
            if (d.parent_inode == 0 && d.inode != root_inode) stack.push_back(d.inode);
        }
        while (!stack.empty()) {
            uint64_t cur = stack.back();
            stack.pop_back();
            order.push_back(cur);
            auto cit = children.find(cur);
            if (cit != children.end()) {
                for (auto child : cit->second) {
                    stack.push_back(child);
                }
            }
        }
        std::reverse(order.begin(), order.end());
    }

    for (uint64_t inode : order) {
        auto files = get_files_by_dir(inode);

        // Error files are treated as uncovered for coverage purposes.
        // Only dirs.error flag (scanner-level error) forces a directory to Error state.
        int total = static_cast<int>(files.size());
        int cov   = 0;
        for (const auto& f : files) {
            if (f.covered) ++cov;
        }

        auto cit = children.find(inode);
        if (cit != children.end()) {
            for (uint64_t child_inode : cit->second) {
                auto rit = result.find(child_inode);
                if (rit != result.end()) {
                    int child_state = rit->second;
                    if (child_state == static_cast<int>(CoveredState::Empty)) {
                        // Empty child: skip – does not affect parent coverage
                    } else if (child_state == static_cast<int>(CoveredState::Error)) {
                        // Child with error: treat as uncovered + additional penalty
                        // to ensure parent is never considered covered or empty
                        total += 2;
                        cov   += 1;
                    } else if (child_state == static_cast<int>(CoveredState::Covered)) {
                        ++total;
                        ++cov;
                    } else if (child_state == static_cast<int>(CoveredState::Partial)) {
                        total += 2;
                        cov   += 1;
                    } else {
                        ++total;
                    }
                }
            }
        }

        // Only the scanner-level dir error flag forces Error state.
        // Error files just count as uncovered (cov stays 0 for them).
        auto dit = std::find_if(dirs.begin(), dirs.end(), [inode](const DirEntry& d) { return d.inode == inode; });
        bool dir_has_error = (dit != dirs.end() && dit->error);

        int state;
        if (dir_has_error) {
            // Scanner could not open this directory – unknown contents.
            state = static_cast<int>(CoveredState::Error);
        } else if (total == 0) {
            state = static_cast<int>(CoveredState::Empty);
        } else if (cov == 0) {
            state = static_cast<int>(CoveredState::Uncovered);
        } else if (cov >= total) {
            state = static_cast<int>(CoveredState::Covered);
        } else {
            state = static_cast<int>(CoveredState::Partial);
        }
        result[inode] = state;
    }

    return result;
}

// ------------------------------------------------------------------
// HashDatabase implementation
// ------------------------------------------------------------------

HashDatabase::HashDatabase(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    exec_sql(db_, "PRAGMA journal_mode=WAL;");
    exec_sql(db_, "PRAGMA synchronous=NORMAL;");

    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS hashes (
            inode     INTEGER PRIMARY KEY,
            head_hash BLOB,
            full_hash BLOB
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS cluster_log (
            size          INTEGER PRIMARY KEY,
            file_count    INTEGER NOT NULL,
            matched       INTEGER NOT NULL,
            covered_count INTEGER NOT NULL
        ) WITHOUT ROWID;
    )";
    rc = exec_sql(db_, schema);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    // Index on full_hash for fast reverse lookup (find file by hash)
    exec_sql(db_, "CREATE INDEX IF NOT EXISTS idx_hashes_full_hash ON hashes(full_hash);");

    const char* sql_head = "INSERT INTO hashes (inode, head_hash) VALUES (?, ?) ON CONFLICT(inode) DO UPDATE SET head_hash = excluded.head_hash";
    rc = sqlite3_prepare_v2(db_, sql_head, -1, &stmt_set_head_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    const char* sql_full = "INSERT INTO hashes (inode, full_hash) VALUES (?, ?) ON CONFLICT(inode) DO UPDATE SET full_hash = excluded.full_hash";
    rc = sqlite3_prepare_v2(db_, sql_full, -1, &stmt_set_full_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    const char* sql_log = "INSERT OR REPLACE INTO cluster_log (size, file_count, matched, covered_count) VALUES (?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(db_, sql_log, -1, &stmt_log_cluster_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }
}

HashDatabase::~HashDatabase() {
    if (stmt_set_head_) sqlite3_finalize(stmt_set_head_);
    if (stmt_set_full_) sqlite3_finalize(stmt_set_full_);
    if (stmt_log_cluster_) sqlite3_finalize(stmt_log_cluster_);
    if (db_)            sqlite3_close(db_);
}

std::optional<std::vector<uint8_t>> HashDatabase::get_head_hash(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT head_hash FROM hashes WHERE inode = ?";
    std::optional<std::vector<uint8_t>> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int len = sqlite3_column_bytes(stmt, 0);
            if (blob && len > 0) {
                result = std::vector<uint8_t>(static_cast<const uint8_t*>(blob),
                                              static_cast<const uint8_t*>(blob) + len);
            }
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::optional<std::vector<uint8_t>> HashDatabase::get_full_hash(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT full_hash FROM hashes WHERE inode = ?";
    std::optional<std::vector<uint8_t>> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const void* blob = sqlite3_column_blob(stmt, 0);
            int len = sqlite3_column_bytes(stmt, 0);
            if (blob && len > 0) {
                result = std::vector<uint8_t>(static_cast<const uint8_t*>(blob),
                                              static_cast<const uint8_t*>(blob) + len);
            }
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

void HashDatabase::set_head_hash(uint64_t inode, const uint8_t* hash, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stmt_set_head_) return;
    sqlite3_reset(stmt_set_head_);
    sqlite3_bind_int64(stmt_set_head_, 1, static_cast<sqlite3_int64>(inode));
    sqlite3_bind_blob(stmt_set_head_, 2, hash, static_cast<int>(len), SQLITE_STATIC);
    sqlite3_step(stmt_set_head_);
}

void HashDatabase::set_full_hash(uint64_t inode, const uint8_t* hash, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stmt_set_full_) return;
    sqlite3_reset(stmt_set_full_);
    sqlite3_bind_int64(stmt_set_full_, 1, static_cast<sqlite3_int64>(inode));
    sqlite3_bind_blob(stmt_set_full_, 2, hash, static_cast<int>(len), SQLITE_STATIC);
    sqlite3_step(stmt_set_full_);
}

void HashDatabase::log_cluster(int64_t size, uint64_t file_count, bool matched, uint64_t covered_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stmt_log_cluster_) return;
    sqlite3_reset(stmt_log_cluster_);
    sqlite3_bind_int64(stmt_log_cluster_, 1, static_cast<sqlite3_int64>(size));
    sqlite3_bind_int64(stmt_log_cluster_, 2, static_cast<sqlite3_int64>(file_count));
    sqlite3_bind_int(stmt_log_cluster_, 3, matched ? 1 : 0);
    sqlite3_bind_int64(stmt_log_cluster_, 4, static_cast<sqlite3_int64>(covered_count));
    sqlite3_step(stmt_log_cluster_);
}

std::optional<uint64_t> HashDatabase::find_inode_by_full_hash(const std::vector<uint8_t>& full_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT inode FROM hashes WHERE full_hash = ? LIMIT 1";
    std::optional<uint64_t> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_blob(stmt, 1, full_hash.data(),
                          static_cast<int>(full_hash.size()), SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

void HashDatabase::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    }
}

// ------------------------------------------------------------------
// Update-phase helpers
// ------------------------------------------------------------------

std::vector<FileEntry> Database::get_files_in_subtree(uint64_t root_dir_inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<FileEntry> files;

    // Collect all dir inodes under the subtree recursively
    std::vector<uint64_t> dir_stack;
    dir_stack.push_back(root_dir_inode);
    std::unordered_set<uint64_t> visited;
    std::vector<uint64_t> subtree_dirs;

    while (!dir_stack.empty()) {
        uint64_t cur = dir_stack.back();
        dir_stack.pop_back();
        if (visited.count(cur)) continue;
        visited.insert(cur);
        subtree_dirs.push_back(cur);

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM dirs WHERE parent_inode = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cur));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                uint64_t child = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                dir_stack.push_back(child);
            }
            sqlite3_finalize(stmt);
        }
    }

    // Now get all files for all subtree dirs
    for (uint64_t dir_inode : subtree_dirs) {
        auto dir_files = get_files_by_dir(dir_inode);
        files.insert(files.end(), dir_files.begin(), dir_files.end());
    }
    return files;
}

std::vector<DirEntry> Database::get_dirs_in_subtree(uint64_t root_dir_inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DirEntry> dirs;

    std::vector<uint64_t> dir_stack;
    dir_stack.push_back(root_dir_inode);
    std::unordered_set<uint64_t> visited;

    while (!dir_stack.empty()) {
        uint64_t cur = dir_stack.back();
        dir_stack.pop_back();
        if (visited.count(cur)) continue;
        visited.insert(cur);

        // Get this dir entry
        auto d = get_dir(cur);
        if (d.has_value()) {
            dirs.push_back(*d);
        }

        // Find children
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM dirs WHERE parent_inode = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cur));
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                uint64_t child = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                dir_stack.push_back(child);
            }
            sqlite3_finalize(stmt);
        }
    }
    return dirs;
}

void Database::set_dir_delta(uint64_t inode, int delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE dirs SET delta = ? WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (delta != 0)
            sqlite3_bind_int(stmt, 1, delta);
        else
            sqlite3_bind_null(stmt, 1);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::set_file_delta(uint64_t dir_inode, const std::string& name, int delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET delta = ? WHERE dir_inode = ? AND name = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (delta != 0)
            sqlite3_bind_int(stmt, 1, delta);
        else
            sqlite3_bind_null(stmt, 1);
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(dir_inode));
        sqlite3_bind_text(stmt, 3, name.c_str(), static_cast<int>(name.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::update_file(size_t dir_inode, const std::string& name, uint64_t inode, int64_t size, int64_t mtime) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET inode = ?, size = ?, mtime = ?, delta = NULL WHERE dir_inode = ? AND name = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(size));
        sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(mtime));
        sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(dir_inode));
        sqlite3_bind_text(stmt, 5, name.c_str(), static_cast<int>(name.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::mark_file_deleted(uint64_t dir_inode, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE files SET delta = 2 WHERE dir_inode = ? AND name = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(dir_inode));
        sqlite3_bind_text(stmt, 2, name.c_str(), static_cast<int>(name.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::mark_dir_deleted(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE dirs SET delta = 2 WHERE inode = ?";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::optional<uint64_t> Database::find_dir_inode(const std::string& abs_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Get root path from config
    auto root = read_config_json();
    if (!root.has_value()) return std::nullopt;

    std::string root_path = *root;
    // Ensure root_path ends with /
    if (!root_path.empty() && root_path.back() != '/') root_path += '/';

    // abs_path must start with root_path
    if (abs_path.compare(0, root_path.size(), root_path) != 0) {
        return std::nullopt;
    }

    std::string rel = abs_path.substr(root_path.size());
    // Remove trailing slash
    while (!rel.empty() && rel.back() == '/') rel.pop_back();

    if (rel.empty()) {
        // We're asking for the root dir itself — find dir with parent_inode = 0
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM dirs WHERE parent_inode IS NULL OR parent_inode = 0 LIMIT 1";
        std::optional<uint64_t> result;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
        return result;
    }

    // Walk the path components
    std::vector<std::string> components;
    {
        std::string token;
        for (char c : rel) {
            if (c == '/') {
                if (!token.empty()) {
                    components.push_back(token);
                    token.clear();
                }
            } else {
                token += c;
            }
        }
        if (!token.empty()) components.push_back(token);
    }

    // Find root dir inode
    uint64_t current_inode = 0;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM dirs WHERE parent_inode IS NULL OR parent_inode = 0 LIMIT 1";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                current_inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            } else {
                sqlite3_finalize(stmt);
                return std::nullopt;
            }
            sqlite3_finalize(stmt);
        } else {
            return std::nullopt;
        }
    }

    // Walk each component
    for (const auto& component : components) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM dirs WHERE parent_inode = ? AND name = ?";
        std::optional<uint64_t> next;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(current_inode));
            sqlite3_bind_text(stmt, 2, component.c_str(), static_cast<int>(component.size()), SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                next = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            }
            sqlite3_finalize(stmt);
        }
        if (!next.has_value()) return std::nullopt;
        current_inode = *next;
    }

    return current_inode;
}

std::optional<FileEntry> Database::get_file(uint64_t dir_inode, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT dir_inode, name, inode, size, mtime, covered, error, backup_id FROM files WHERE dir_inode = ? AND name = ?";
    std::optional<FileEntry> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(dir_inode));
        sqlite3_bind_text(stmt, 2, name.c_str(), static_cast<int>(name.size()), SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int bid = 0;
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL)
                bid = sqlite3_column_int(stmt, 7);
            result = FileEntry{
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 0)),
                reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)),
                static_cast<uint64_t>(sqlite3_column_int64(stmt, 2)),
                sqlite3_column_int64(stmt, 3),
                sqlite3_column_int64(stmt, 4),
                sqlite3_column_int(stmt, 5),
                sqlite3_column_int(stmt, 6),
                bid
            };
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

std::optional<DirEntry> Database::get_dir(uint64_t inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT inode, parent_inode, name, covered, error FROM dirs WHERE inode = ?";
    std::optional<DirEntry> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(inode));
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            DirEntry d;
            d.inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
            if (sqlite3_column_type(stmt, 1) == SQLITE_NULL)
                d.parent_inode = 0;
            else
                d.parent_inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            d.name = name ? name : "";
            d.covered = sqlite3_column_int(stmt, 3);
            d.error = sqlite3_column_int(stmt, 4);
            result = d;
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

} // namespace covered
