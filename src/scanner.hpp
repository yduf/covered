#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <atomic>

#include "db.hpp"

namespace covered {

class Scanner {
public:
    Scanner(Database& db, uint64_t root_dev);

    bool scan(const std::string& root_path);

    uint64_t files_seen() const { return files_seen_.load(); }
    uint64_t dirs_seen()  const { return dirs_seen_.load(); }
    uint64_t skipped()    const { return skipped_.load(); }

private:
    bool scan_dir(int dir_fd, uint64_t dir_inode, const std::string& path);

    Database& db_;
    uint64_t root_dev_;
    std::atomic<uint64_t> files_seen_{0};
    std::atomic<uint64_t> dirs_seen_{0};
    std::atomic<uint64_t> skipped_{0};
};

} // namespace covered
