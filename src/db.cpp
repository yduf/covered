#include "db.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>

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

    // Speed / reliability tuning
    exec_sql(db_, "PRAGMA journal_mode=WAL;");
    exec_sql(db_, "PRAGMA synchronous=NORMAL;");
    exec_sql(db_, "PRAGMA cache_size=-64000;");   // 64 MB
    exec_sql(db_, "PRAGMA temp_store=MEMORY;");
    exec_sql(db_, "PRAGMA mmap_size=30000000000;");

    // Schema
    const char* schema = R"(
        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS dirs (
            inode        INTEGER NOT NULL,
            parent_inode INTEGER,
            name         TEXT    NOT NULL,
            error        INTEGER DEFAULT NULL,
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
            PRIMARY KEY (dir_inode, name)
        ) WITHOUT ROWID;

        CREATE INDEX IF NOT EXISTS idx_files_inode ON files(inode);
        CREATE INDEX IF NOT EXISTS idx_files_size  ON files(size);
    )";

    rc = exec_sql(db_, schema);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    // Prepared statements for bulk insert
    const char* sql_dir = "INSERT INTO dirs (inode, parent_inode, name, error) VALUES (?, ?, ?, ?)";
    rc = sqlite3_prepare_v2(db_, sql_dir, -1, &stmt_dir_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    const char* sql_file = "INSERT INTO files (dir_inode, name, inode, size, mtime, covered, error) VALUES (?, ?, ?, ?, ?, ?, ?)";
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

void Database::set_device(uint64_t device) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO meta (key, value) VALUES ('device', ?)";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(device));
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void Database::set_root_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO meta (key, value) VALUES ('root_path', ?)";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path.c_str(), static_cast<int>(path.size()), SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

std::optional<std::string> Database::get_root_path() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT value FROM meta WHERE key = 'root_path'";
    std::optional<std::string> result;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (text) result = std::string(text);
        }
        sqlite3_finalize(stmt);
    }
    return result;
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
    // Ignore error – columns may already exist (new DBs have them in the schema;
    // existing DBs get them via this ALTER with DEFAULT NULL to match new schema)
    exec_sql(db_, "ALTER TABLE dirs ADD COLUMN error INTEGER DEFAULT NULL;");
    exec_sql(db_, "ALTER TABLE files ADD COLUMN error INTEGER DEFAULT NULL;");
}

std::vector<DirEntry> Database::get_all_dirs() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DirEntry> dirs;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT inode, parent_inode, name, covered, error FROM dirs";
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
    const char* sql = "SELECT dir_inode, name, inode, size, mtime, covered, error FROM files";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
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

void HashDatabase::sync() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    }
}

} // namespace covered