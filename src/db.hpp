#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <tuple>

#include <sqlite3.h>

namespace covered {

// covered value for dirs (3-state)
enum class CoveredState : int {
    Uncovered = 0,
    Covered   = 1,
    Partial   = 2,
    Empty     = 3
};

struct DirEntry {
    uint64_t inode;
    uint64_t parent_inode;  // 0 for root (no dir has inode 0)
    std::string name;
    int covered = 0; // CoveredState, used for reporting
};

struct FileEntry {
    uint64_t dir_inode;
    std::string name;
    uint64_t inode;
    int64_t  size;
    int64_t  mtime;  // seconds since epoch
    int      covered; // used for reporting at the end
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    // not copyable / movable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void set_device(uint64_t device);
    void set_root_path(const std::string& path);
    std::optional<std::string> get_root_path();

    void begin_batch();
    void add_dir(const DirEntry& d);
    void add_file(const FileEntry& f);
    void commit_batch();
    void sync();

    // Match-phase helpers
    std::vector<int64_t> get_distinct_sizes();
    std::vector<FileEntry> get_files_by_size(int64_t size);
    uint64_t count_files();
    void set_covered(uint64_t inode, int covered);

    // Report-phase helpers
    void migrate_dirs_covered_column();
    std::vector<DirEntry> get_all_dirs();
    std::vector<FileEntry> get_files_by_dir(uint64_t dir_inode);
    std::vector<FileEntry> get_all_files();
    void set_dir_covered(uint64_t inode, int covered);

    // Shared algorithm: compute covered state for all directories (bottom-up)
    // Returns a map inode -> CoveredState
    std::unordered_map<uint64_t, int> compute_dir_covered();

    // Build a map inode -> full path from the dirs table
    std::unordered_map<uint64_t, std::string> build_dir_paths(const std::string& root_path);

    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

    sqlite3* raw_db() const { return db_; }

private:
    void flush_dirs();
    void flush_files();

    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_dir_ = nullptr;
    sqlite3_stmt* stmt_file_ = nullptr;

    std::vector<DirEntry>  dir_buffer_;
    std::vector<FileEntry> file_buffer_;
    static constexpr size_t BATCH_SIZE = 10000;

    bool error_ = false;
    std::string error_msg_;
    std::mutex mutex_;
};

// ------------------------------------------------------------------
// HashDatabase: manages <folder>/hash.db (inode -> head_hash, full_hash)
// ------------------------------------------------------------------
class HashDatabase {
public:
    explicit HashDatabase(const std::string& path);
    ~HashDatabase();

    HashDatabase(const HashDatabase&) = delete;
    HashDatabase& operator=(const HashDatabase&) = delete;

    std::optional<std::vector<uint8_t>> get_head_hash(uint64_t inode);
    std::optional<std::vector<uint8_t>> get_full_hash(uint64_t inode);

    void set_head_hash(uint64_t inode, const uint8_t* hash, size_t len);
    void set_full_hash(uint64_t inode, const uint8_t* hash, size_t len);

    void log_cluster(int64_t size, uint64_t file_count, bool matched, uint64_t covered_count);
    void sync();

    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_set_head_ = nullptr;
    sqlite3_stmt* stmt_set_full_ = nullptr;
    sqlite3_stmt* stmt_log_cluster_ = nullptr;
    bool error_ = false;
    std::string error_msg_;
    std::mutex mutex_;
};

} // namespace covered
