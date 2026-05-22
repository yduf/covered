#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include "db.hpp"

namespace covered {

class Scanner {
public:
    Scanner(Database& db, uint64_t root_dev,
            HashDatabase* hash_db = nullptr);

    bool scan(const std::string& root_path);

    uint64_t files_seen() const { return files_seen_; }
    uint64_t dirs_seen()  const { return dirs_seen_; }
    uint64_t skipped()    const { return skipped_; }
    uint64_t head_hashes_computed() const { return head_hashes_computed_; }
    uint64_t full_hashes_computed() const { return full_hashes_computed_; }

private:
    bool scan_dir(int dir_fd, uint64_t dir_inode, const std::string& path);

    void compute_file_hashes(uint64_t inode, const std::string& path, int64_t size);

    Database& db_;
    HashDatabase* hash_db_;
    uint64_t root_dev_;
    uint64_t files_seen_ = 0;
    uint64_t dirs_seen_  = 0;
    uint64_t skipped_    = 0;
    uint64_t head_hashes_computed_ = 0;
    uint64_t full_hashes_computed_ = 0;
};

} // namespace covered
