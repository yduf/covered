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

// covered value for dirs (5-state)
enum class CoveredState : int {
    Uncovered = 0,
    Covered   = 1,
    Partial   = 2,
    Empty     = 3,  // dir (and all sub-dirs) contain no files at all
    Error     = 4,  // dir or file could not be accessed (permission, IO error)
};

// delta value for update tracking
enum class DeltaState : int {
    Unchanged = 0,
    New       = 1,
    Deleted   = 2,
};

struct DirEntry {
    uint64_t inode;
    uint64_t parent_inode;  // 0 for root (no dir has inode 0)
    std::string name;
    int covered = 0;        // CoveredState, used for reporting
    int error   = 0;        // 1 if directory could not be scanned (permission etc.)
    int delta   = 0;        // DeltaState: 0=unchanged, 1=new, 2=deleted
};

struct FileEntry {
    uint64_t dir_inode;
    std::string name;
    uint64_t inode;
    int64_t  size;
    int64_t  mtime;         // seconds since epoch
    int      covered;       // used for reporting at the end
    int      error;         // 1 if file could not be accessed (permission etc.)
    int      backup_id = 0; // id of the backup_db that matched this file (0 = none)
    int      delta   = 0;   // DeltaState: 0=unchanged, 1=new, 2=deleted
};

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();

    // not copyable / movable
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

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
    void set_file_backup_id(uint64_t inode, int backup_id);
    void set_file_error(uint64_t inode);

    // Backup path lookup
    const std::unordered_map<int, std::string>& get_backup_paths() const { return backup_paths_; }

    // Report-phase helpers
    void migrate_dirs_covered_column();
    void migrate_error_columns();
    void migrate_backup_id_column();
    void migrate_backup_db_table();
    void migrate_drop_meta_table();
    void migrate_delta_columns();
    int  register_backup_db(const std::string& backup_path);
    std::string get_backup_path(int backup_id);
    std::vector<DirEntry> get_all_dirs();
    std::vector<FileEntry> get_files_by_dir(uint64_t dir_inode);
    std::vector<FileEntry> get_all_files();
    std::vector<FileEntry> get_files_in_subtree(uint64_t root_dir_inode);
    std::vector<DirEntry> get_dirs_in_subtree(uint64_t root_dir_inode);
    void set_dir_covered(uint64_t inode, int covered);
    void set_dir_delta(uint64_t inode, int delta);
    void set_file_delta(uint64_t dir_inode, const std::string& name, int delta);
    void update_file(size_t dir_inode, const std::string& name, uint64_t inode, int64_t size, int64_t mtime);
    void mark_file_deleted(uint64_t dir_inode, const std::string& name);
    void mark_dir_deleted(uint64_t inode);
    std::optional<uint64_t> find_dir_inode(const std::string& abs_path);
    std::optional<FileEntry> get_file(uint64_t dir_inode, const std::string& name);
    std::optional<DirEntry> get_dir(uint64_t inode);


    // Shared algorithm: compute covered state for all directories (bottom-up)
    // Returns a map inode -> CoveredState
    std::unordered_map<uint64_t, int> compute_dir_covered();

    // Build a map inode -> full path from the dirs table
    std::unordered_map<uint64_t, std::string> build_dir_paths(const std::string& root_path);

    // Backing field for get_backup_paths()
    std::unordered_map<int, std::string> backup_paths_;

    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

    sqlite3* raw_db() const { return db_; }

public:
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
    std::string db_folder_;
    std::optional<std::string> read_config_json() const;
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

    // Reverse lookup: find inode by full_hash blob
    std::optional<uint64_t> find_inode_by_full_hash(const std::vector<uint8_t>& full_hash);

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