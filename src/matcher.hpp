#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "db.hpp"

namespace covered {

class Matcher {
public:
    Matcher(Database& src_db, HashDatabase& src_hash,
            Database& bkp_db, HashDatabase& bkp_hash,
            const std::string& src_root, const std::string& bkp_root,
            int backup_id, bool debug = false);

    bool run();

    uint64_t clusters_processed() const { return clusters_processed_; }
    uint64_t files_covered()      const { return files_covered_; }
    uint64_t files_checked()      const { return files_checked_; }
    uint64_t head_hashes_computed() const { return head_hashes_computed_; }
    uint64_t full_hashes_computed() const { return full_hashes_computed_; }
    uint64_t total_src_files()      const { return total_src_files_; }

private:
    struct FileInfo {
        uint64_t inode;
        uint64_t dir_inode;
        std::string name;
        int64_t size;
    };

    std::string build_path(Database& db, uint64_t dir_inode, const std::string& name,
                           const std::string& root);
    void compute_head_hashes(Database& db, HashDatabase& hash_db,
                             const std::vector<FileInfo>& files, const std::string& root,
                             const std::string& db_type);
    void compute_full_hashes(Database& db, HashDatabase& hash_db,
                             const std::vector<FileInfo>& files, const std::string& root);

    Database& src_db_;
    HashDatabase& src_hash_;
    Database& bkp_db_;
    HashDatabase& bkp_hash_;
    std::string src_root_;
    std::string bkp_root_;
    int backup_id_ = 0;

    struct DirCache {
        std::unordered_map<uint64_t, std::string> name;
        std::unordered_map<uint64_t, uint64_t> parent;
    };

    DirCache& get_dir_cache(Database& db);

    DirCache src_dir_cache_;
    DirCache bkp_dir_cache_;

    uint64_t clusters_processed_ = 0;
    uint64_t files_covered_ = 0;
    uint64_t files_checked_ = 0;
    uint64_t head_hashes_computed_ = 0;
    uint64_t full_hashes_computed_ = 0;
    uint64_t total_src_files_ = 0;
    bool debug_ = false;

    static std::string hash_to_hex(const uint8_t* hash, size_t len);
};

} // namespace covered
