#include "matcher.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cerrno>

#include "blake3.h"

namespace covered {

static constexpr size_t HEAD_SIZE = 2048;
static constexpr size_t BLAKE3_HASH_LEN = BLAKE3_OUT_LEN; // 32 bytes

Matcher::Matcher(Database& src_db, HashDatabase& src_hash,
                 Database& bkp_db, HashDatabase& bkp_hash,
                 const std::string& src_root, const std::string& bkp_root)
    : src_db_(src_db), src_hash_(src_hash),
      bkp_db_(bkp_db), bkp_hash_(bkp_hash),
      src_root_(src_root), bkp_root_(bkp_root) {}

std::string Matcher::build_path(Database& db, uint64_t dir_inode,
                                const std::string& name, const std::string& root) {
    // Build path from dir_inode upward using cached dir info
    std::vector<std::string> parts;
    parts.push_back(name);

    uint64_t cur = dir_inode;
    while (cur != 0) {
        auto it_name = dir_name_cache_.find(cur);
        auto it_parent = dir_parent_cache_.find(cur);

        if (it_name == dir_name_cache_.end() || it_parent == dir_parent_cache_.end()) {
            // Query from DB
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT parent_inode, name FROM dirs WHERE inode = ?";
            if (sqlite3_prepare_v2(db.raw_db(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cur));
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    uint64_t parent = sqlite3_column_type(stmt, 0) == SQLITE_NULL
                                          ? 0
                                          : static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    dir_parent_cache_[cur] = parent;
                    dir_name_cache_[cur] = n ? n : "";
                    it_name = dir_name_cache_.find(cur);
                    it_parent = dir_parent_cache_.find(cur);
                }
                sqlite3_finalize(stmt);
            }
            if (it_name == dir_name_cache_.end()) {
                break; // shouldn't happen
            }
        }

        if (cur == 0) break;
        // Don't include the root directory name (already in root path)
        if (it_parent->second == 0) break;
        parts.push_back(it_name->second);
        cur = it_parent->second;
    }

    std::string path = root;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += *it;
        if (it + 1 != parts.rend()) {
            path += '/';
        }
    }
    return path;
}

void Matcher::compute_head_hashes(Database& db, HashDatabase& hash_db,
                                  const std::vector<FileInfo>& files,
                                  const std::string& root) {
    for (const auto& f : files) {
        auto existing = hash_db.get_head_hash(f.inode);
        if (existing.has_value()) continue;

        std::string path = build_path(db, f.dir_inode, f.name, root);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::cerr << "Warning: cannot open '" << path << "': " << std::strerror(errno) << "\n";
            continue;
        }

        char buffer[HEAD_SIZE];
        file.read(buffer, HEAD_SIZE);
        auto bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read == 0) continue;

        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, buffer, bytes_read);

        uint8_t hash[BLAKE3_HASH_LEN];
        blake3_hasher_finalize(&hasher, hash, BLAKE3_HASH_LEN);
        hash_db.set_head_hash(f.inode, hash, BLAKE3_HASH_LEN);
    }
}

void Matcher::compute_full_hashes(Database& db, HashDatabase& hash_db,
                                  const std::vector<FileInfo>& files,
                                  const std::string& root) {
    for (const auto& f : files) {
        auto existing = hash_db.get_full_hash(f.inode);
        if (existing.has_value()) continue;

        std::string path = build_path(db, f.dir_inode, f.name, root);
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::cerr << "Warning: cannot open '" << path << "': " << std::strerror(errno) << "\n";
            continue;
        }

        blake3_hasher hasher;
        blake3_hasher_init(&hasher);

        char buffer[65536];
        while (file.good()) {
            file.read(buffer, sizeof(buffer));
            auto bytes_read = static_cast<size_t>(file.gcount());
            if (bytes_read > 0) {
                blake3_hasher_update(&hasher, buffer, bytes_read);
            }
        }

        uint8_t hash[BLAKE3_HASH_LEN];
        blake3_hasher_finalize(&hasher, hash, BLAKE3_HASH_LEN);
        hash_db.set_full_hash(f.inode, hash, BLAKE3_HASH_LEN);
    }
}

bool Matcher::run() {
    // Load all dirs into cache for both DBs
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode, parent_inode, name FROM dirs";
        if (sqlite3_prepare_v2(src_db_.raw_db(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                uint64_t inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                uint64_t parent = sqlite3_column_type(stmt, 1) == SQLITE_NULL
                                      ? 0
                                      : static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                dir_name_cache_[inode] = name ? name : "";
                dir_parent_cache_[inode] = parent;
            }
            sqlite3_finalize(stmt);
        }
    }
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode, parent_inode, name FROM dirs";
        if (sqlite3_prepare_v2(bkp_db_.raw_db(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                uint64_t inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                uint64_t parent = sqlite3_column_type(stmt, 1) == SQLITE_NULL
                                      ? 0
                                      : static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                dir_name_cache_[inode] = name ? name : "";
                dir_parent_cache_[inode] = parent;
            }
            sqlite3_finalize(stmt);
        }
    }

    auto sizes = src_db_.get_distinct_sizes();
    if (sizes.empty()) {
        std::cout << "No files to match in source.\n";
        return true;
    }

    std::cout << "Matching " << sizes.size() << " size clusters...\n";

    for (size_t idx = 0; idx < sizes.size(); ++idx) {
        int64_t size = sizes[idx];

        auto src_entries = src_db_.get_files_by_size(size);
        auto bkp_entries = bkp_db_.get_files_by_size(size);

        if (bkp_entries.empty()) continue;

        // Convert to FileInfo
        std::vector<FileInfo> src_files;
        src_files.reserve(src_entries.size());
        for (const auto& e : src_entries) {
            src_files.push_back({e.inode, e.dir_inode, e.name});
        }
        std::vector<FileInfo> bkp_files;
        bkp_files.reserve(bkp_entries.size());
        for (const auto& e : bkp_entries) {
            bkp_files.push_back({e.inode, e.dir_inode, e.name});
        }

        // Step 1: compute head hashes
        compute_head_hashes(src_db_, src_hash_, src_files, src_root_);
        compute_head_hashes(bkp_db_, bkp_hash_, bkp_files, bkp_root_);

        // Step 2: build hash -> inode map for source, collect backup hashes
        std::unordered_map<std::string, std::vector<uint64_t>> src_head_map;
        for (const auto& f : src_files) {
            auto h = src_hash_.get_head_hash(f.inode);
            if (h.has_value()) {
                src_head_map[std::string(reinterpret_cast<const char*>(h->data()), h->size())].push_back(f.inode);
            }
        }

        std::unordered_set<std::string> bkp_head_set;
        for (const auto& f : bkp_files) {
            auto h = bkp_hash_.get_head_hash(f.inode);
            if (h.has_value()) {
                bkp_head_set.insert(std::string(reinterpret_cast<const char*>(h->data()), h->size()));
            }
        }

        // Find matching head hashes
        std::vector<uint64_t> src_matched_inodes;
        std::vector<uint64_t> bkp_matched_inodes;
        for (const auto& f : bkp_files) {
            auto h = bkp_hash_.get_head_hash(f.inode);
            if (h.has_value()) {
                std::string key(reinterpret_cast<const char*>(h->data()), h->size());
                if (src_head_map.count(key)) {
                    bkp_matched_inodes.push_back(f.inode);
                    for (uint64_t inode : src_head_map[key]) {
                        src_matched_inodes.push_back(inode);
                    }
                }
            }
        }

        if (src_matched_inodes.empty()) continue;

        // Step 3: compute full hashes for matched files
        std::vector<FileInfo> src_to_full;
        std::vector<FileInfo> bkp_to_full;
        for (const auto& f : src_files) {
            if (std::find(src_matched_inodes.begin(), src_matched_inodes.end(), f.inode) != src_matched_inodes.end()) {
                src_to_full.push_back(f);
            }
        }
        for (const auto& f : bkp_files) {
            if (std::find(bkp_matched_inodes.begin(), bkp_matched_inodes.end(), f.inode) != bkp_matched_inodes.end()) {
                bkp_to_full.push_back(f);
            }
        }

        compute_full_hashes(src_db_, src_hash_, src_to_full, src_root_);
        compute_full_hashes(bkp_db_, bkp_hash_, bkp_to_full, bkp_root_);

        // Step 4: collect backup full hashes, mark covered source files
        std::unordered_set<std::string> bkp_full_set;
        for (uint64_t inode : bkp_matched_inodes) {
            auto h = bkp_hash_.get_full_hash(inode);
            if (h.has_value()) {
                bkp_full_set.insert(std::string(reinterpret_cast<const char*>(h->data()), h->size()));
            }
        }

        for (uint64_t inode : src_matched_inodes) {
            auto h = src_hash_.get_full_hash(inode);
            if (h.has_value()) {
                std::string key(reinterpret_cast<const char*>(h->data()), h->size());
                if (bkp_full_set.count(key)) {
                    src_db_.set_covered(inode, 1);
                    files_covered_++;
                }
            }
        }

        clusters_processed_++;
        files_checked_ += src_files.size();

        if ((idx + 1) % 100 == 0 || idx + 1 == sizes.size()) {
            std::cout << "\rProcessed " << (idx + 1) << "/" << sizes.size()
                      << " clusters, " << files_covered_ << " files covered"
                      << std::flush;
        }
    }

    std::cout << "\nDone. " << clusters_processed_ << " clusters matched, "
              << files_covered_ << " source files covered.\n";
    return true;
}

} // namespace covered
