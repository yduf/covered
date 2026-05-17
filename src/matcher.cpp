#include "matcher.hpp"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <iomanip>

#include "blake3.h"

namespace covered {

static constexpr size_t HEAD_SIZE = 2048;
static constexpr size_t BLAKE3_HASH_LEN = BLAKE3_OUT_LEN; // 32 bytes

Matcher::Matcher(Database& src_db, HashDatabase& src_hash,
                 Database& bkp_db, HashDatabase& bkp_hash,
                 const std::string& src_root, const std::string& bkp_root,
                 bool debug)
    : src_db_(src_db), src_hash_(src_hash),
      bkp_db_(bkp_db), bkp_hash_(bkp_hash),
      src_root_(src_root), bkp_root_(bkp_root), debug_(debug) {}

Matcher::DirCache& Matcher::get_dir_cache(Database& db) {
    return (&db == &src_db_) ? src_dir_cache_ : bkp_dir_cache_;
}

std::string Matcher::hash_to_hex(const uint8_t* hash, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(hex[hash[i] >> 4]);
        out.push_back(hex[hash[i] & 0x0f]);
    }
    return out;
}

std::string Matcher::build_path(Database& db, uint64_t dir_inode,
                                const std::string& name, const std::string& root) {
    // Build path from dir_inode upward using cached dir info
    std::vector<std::string> parts;
    parts.push_back(name);

    auto& cache = get_dir_cache(db);

    uint64_t cur = dir_inode;
    while (cur != 0) {
        auto it_name = cache.name.find(cur);
        auto it_parent = cache.parent.find(cur);

        if (it_name == cache.name.end() || it_parent == cache.parent.end()) {
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
                    cache.parent[cur] = parent;
                    cache.name[cur] = n ? n : "";
                    it_name = cache.name.find(cur);
                    it_parent = cache.parent.find(cur);
                }
                sqlite3_finalize(stmt);
            }
            if (it_name == cache.name.end()) {
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
                                  const std::string& root,
                                  const std::string& db_type
                                ) {
    for (const auto& f : files) {
        auto t0 = std::chrono::steady_clock::now();
        auto existing = hash_db.get_head_hash(f.inode);
        bool cached = existing.has_value();
        std::string path;
        if (debug_ || !cached) {
            path = build_path(db, f.dir_inode, f.name, root);
        }

        if (!cached) {
            std::ifstream file(path, std::ios::binary);
            if (!file) {
                std::cerr << "Warning: cannot open " << db_type << " '" << path << "': " << std::strerror(errno) << "\n";
                continue;
            }

            char buffer[HEAD_SIZE];
            file.read(buffer, HEAD_SIZE);
            auto bytes_read = static_cast<size_t>(file.gcount());
            if (bytes_read == 0) {
                if (debug_) {
                    auto t1 = std::chrono::steady_clock::now();
                    auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                    std::cout << "[DEBUG] head_hash " << path
                              << " size=" << f.size
                              << " hash=(empty)"
                              << " computed"
                              << " time=" << dur << "us\n";
                }
                continue;
            }

            blake3_hasher hasher;
            blake3_hasher_init(&hasher);
            blake3_hasher_update(&hasher, buffer, bytes_read);

            uint8_t hash[BLAKE3_HASH_LEN];
            blake3_hasher_finalize(&hasher, hash, BLAKE3_HASH_LEN);
            hash_db.set_head_hash(f.inode, hash, BLAKE3_HASH_LEN);
            head_hashes_computed_++;
            existing = std::vector<uint8_t>( std::begin(hash), std::end(hash));
        }

        if (debug_) {
            auto s_cached = cached ? "cached" : "computed";
            auto t1 = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            std::cout << "[DEBUG] #" <<  f.size << " " 
                        << "head_hash " << db_type <<  " (" << s_cached << ")="  << hash_to_hex(existing->data(), existing->size())
                        << " '" << path << "'"
                        << " time=" << dur << "us\n"
                        ;
        }
    }
}

void Matcher::compute_full_hashes(Database& db, HashDatabase& hash_db,
                                  const std::vector<FileInfo>& files,
                                  const std::string& root) {
    for (const auto& f : files) {
        auto t0 = std::chrono::steady_clock::now();
        auto existing = hash_db.get_full_hash(f.inode);
        bool cached = existing.has_value();
        std::string path;
        if (debug_ || !cached) {
            path = build_path(db, f.dir_inode, f.name, root);
        }

        if (!cached) {
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
            full_hashes_computed_++;

            if (debug_) {
                auto t1 = std::chrono::steady_clock::now();
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                std::cout << "[DEBUG] full_hash " << path
                          << " size=" << f.size
                          << " hash=" << hash_to_hex(hash, BLAKE3_HASH_LEN)
                          << " computed"
                          << " time=" << dur << "us\n";
            }
        } else {
            if (debug_) {
                auto t1 = std::chrono::steady_clock::now();
                auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                std::cout << "[DEBUG] full_hash " << path
                          << " size=" << f.size
                          << " hash=" << hash_to_hex(existing->data(), existing->size())
                          << " cached"
                          << " time=" << dur << "us\n";
            }
        }
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
                src_dir_cache_.name[inode] = name ? name : "";
                src_dir_cache_.parent[inode] = parent;
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
                bkp_dir_cache_.name[inode] = name ? name : "";
                bkp_dir_cache_.parent[inode] = parent;
            }
            sqlite3_finalize(stmt);
        }
    }

    auto sizes = src_db_.get_distinct_sizes();
    if (sizes.empty()) {
        std::cout << "No files to match in source.\n";
        return true;
    }

    total_src_files_ = src_db_.count_files();

    std::cout << "Matching " << sizes.size() << " size clusters...\n";

    using namespace std::chrono;
    auto last_print = steady_clock::now();
    const auto print_interval = std::chrono::seconds(2);

    for (size_t idx = 0; idx < sizes.size(); ++idx) {
        int64_t size = sizes[idx];

        auto src_entries = src_db_.get_files_by_size(size);
        auto bkp_entries = bkp_db_.get_files_by_size(size);
        bool matched = !bkp_entries.empty();
        uint64_t covered_in_cluster = 0;

        if (!matched) {
            src_hash_.log_cluster(size, src_entries.size(), false, 0);
            continue;
        }

        if (debug_) {
            std::cout << "[DEBUG] bucket size=" << size
                      << " src=" << src_entries.size()
                      << " bkp=" << bkp_entries.size() << "\n";
        }

        // Convert to FileInfo
        std::vector<FileInfo> src_files;
        src_files.reserve(src_entries.size());
        for (const auto& e : src_entries) {
            src_files.push_back({e.inode, e.dir_inode, e.name, e.size});
        }
        std::vector<FileInfo> bkp_files;
        bkp_files.reserve(bkp_entries.size());
        for (const auto& e : bkp_entries) {
            bkp_files.push_back({e.inode, e.dir_inode, e.name, e.size});
        }

        // Step 1: compute head hashes
        compute_head_hashes(src_db_, src_hash_, src_files, src_root_, "src");
        compute_head_hashes(bkp_db_, bkp_hash_, bkp_files, bkp_root_, "bak");

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

        if (src_matched_inodes.empty()) {
            src_hash_.log_cluster(size, src_files.size(), true, 0);
            continue;
        }

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
                bool covered = bkp_full_set.count(key) != 0;
                if (covered) {
                    src_db_.set_covered(inode, 1);
                    files_covered_++;
                    covered_in_cluster++;
                }
                if (debug_) {
                    auto it = std::find_if(src_files.begin(), src_files.end(),
                        [inode](const FileInfo& f){ return f.inode == inode; });
                    if (it != src_files.end()) {
                        std::string path = build_path(src_db_, it->dir_inode, it->name, src_root_);
                        std::cout << "[DEBUG] coverage " << path
                                  << " size=" << it->size
                                  << " full_hash=" << hash_to_hex(h->data(), h->size())
                                  << " result=" << (covered ? "covered" : "not_covered") << "\n";
                    }
                }
            }
        }

        clusters_processed_++;
        files_checked_ += src_files.size();
        src_hash_.log_cluster(size, src_files.size(), true, covered_in_cluster);

        auto now = steady_clock::now();
        bool is_last = (idx + 1 == sizes.size());
        if (is_last || now - last_print >= print_interval) {
            std::cout << "\rCluster " << (idx + 1) << "/" << sizes.size()
                      << " (size=" << size << ", n=" << src_files.size() << ")"
                      << "  files=" << files_checked_ << "/" << total_src_files_
                      << "  head=" << head_hashes_computed_
                      << " full=" << full_hashes_computed_
                      << " covered=" << files_covered_
                      << std::flush;
            last_print = now;
        }
    }

    std::cout << "\nDone. " << clusters_processed_ << " clusters matched, "
              << files_covered_ << " source files covered.\n"
              << "  head hashes computed: " << head_hashes_computed_ << "\n"
              << "  full hashes computed: " << full_hashes_computed_ << "\n";
    return true;
}

} // namespace covered
