#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>

#include <sqlite3.h>

namespace covered {

struct DirEntry {
    uint64_t inode;
    uint64_t parent_inode;  // 0 for root (no dir has inode 0)
    std::string name;
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

    void begin_batch();
    void add_dir(const DirEntry& d);
    void add_file(const FileEntry& f);
    void commit_batch();

    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

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

} // namespace covered
