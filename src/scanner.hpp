#pragma once

#include <string>
#include <cstdint>
#include "db.hpp"

namespace covered {

class Scanner {
public:
    Scanner(Database& db, uint64_t root_dev);

    bool scan(const std::string& root_path);

    uint64_t files_seen() const { return files_seen_; }
    uint64_t dirs_seen()  const { return dirs_seen_; }
    uint64_t skipped()    const { return skipped_; }

private:
    bool scan_dir(int dir_fd, uint64_t dir_inode, const std::string& path);

    Database& db_;
    uint64_t root_dev_;
    uint64_t files_seen_ = 0;
    uint64_t dirs_seen_  = 0;
    uint64_t skipped_    = 0;
};

} // namespace covered
