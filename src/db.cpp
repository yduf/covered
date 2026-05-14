#include "db.hpp"

#include <cstdio>
#include <cstring>

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
            PRIMARY KEY (inode)
        ) WITHOUT ROWID;

        CREATE TABLE IF NOT EXISTS files (
            dir_inode  INTEGER NOT NULL,
            name       TEXT    NOT NULL,
            inode      INTEGER NOT NULL,
            size       INTEGER NOT NULL,
            mtime      INTEGER NOT NULL,
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
    const char* sql_dir = "INSERT INTO dirs (inode, parent_inode, name) VALUES (?, ?, ?)";
    rc = sqlite3_prepare_v2(db_, sql_dir, -1, &stmt_dir_, nullptr);
    if (rc != SQLITE_OK) {
        error_ = true;
        error_msg_ = sqlite3_errmsg(db_);
        return;
    }

    const char* sql_file = "INSERT INTO files (dir_inode, name, inode, size, mtime) VALUES (?, ?, ?, ?, ?)";
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
        int rc = sqlite3_step(stmt_file_);
        if (rc != SQLITE_DONE) {
            error_ = true;
            error_msg_ = sqlite3_errmsg(db_);
            break;
        }
    }
    file_buffer_.clear();
}

} // namespace covered
