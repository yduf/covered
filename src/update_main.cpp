#include "commands.hpp"

#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <memory>

#include <nlohmann/json.hpp>

#include "db.hpp"
#include "scanner.hpp"
#include "blake3.h"

static constexpr size_t HEAD_SIZE = 2048;
static constexpr size_t BLAKE3_HASH_LEN = BLAKE3_OUT_LEN; // 32 bytes

static void compute_hashes_for_file(covered::HashDatabase& hash_db, const std::string& path, int64_t size, uint64_t inode) {
    if (size == 0) return;

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "\nWarning: cannot open '" << path << "' for hash: " << std::strerror(errno) << "\n";
        return;
    }

    // Compute head hash
    {
        char buffer[HEAD_SIZE];
        file.read(buffer, HEAD_SIZE);
        auto bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read > 0) {
            uint8_t head_hash[BLAKE3_HASH_LEN];
            blake3_hasher hasher_head;
            blake3_hasher_init(&hasher_head);
            blake3_hasher_update(&hasher_head, buffer, bytes_read);
            blake3_hasher_finalize(&hasher_head, head_hash, BLAKE3_HASH_LEN);
            hash_db.set_head_hash(inode, head_hash, BLAKE3_HASH_LEN);
        }
    }

    // Compute full hash
    file.clear();
    file.seekg(0, std::ios::beg);
    if (!file) {
        std::cerr << "\nWarning: cannot re-read '" << path << "' for full hash: " << std::strerror(errno) << "\n";
        return;
    }

    {
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
        uint8_t full_hash[BLAKE3_HASH_LEN];
        blake3_hasher_finalize(&hasher, full_hash, BLAKE3_HASH_LEN);
        hash_db.set_full_hash(inode, full_hash, BLAKE3_HASH_LEN);
    }
}

struct UpdateCounts {
    uint64_t files_new = 0;
    uint64_t files_deleted = 0;
    uint64_t files_updated = 0;
    uint64_t dirs_new = 0;
    uint64_t dirs_deleted = 0;
    uint64_t hashes_computed = 0;
};

// Recursively update a directory. Returns false on fatal error.
static bool update_dir(covered::Database& db, covered::HashDatabase* hash_db,
                       int dir_fd, uint64_t dir_inode, uint64_t /*parent_inode*/,
                       const std::string& dir_path, bool compute_hash,
                       UpdateCounts& counts,
                       std::unordered_set<uint64_t>& seen_dir_inodes,
                       std::unordered_map<uint64_t, std::unordered_set<std::string>>& seen_files_in_dir) {
    DIR* dir = fdopendir(dir_fd);
    if (!dir) {
        close(dir_fd);
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string sub_path = dir_path;
        if (!sub_path.empty() && sub_path.back() != '/') {
            sub_path += '/';
        }
        sub_path += entry->d_name;

        struct stat st;
        if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            std::cerr << "\nWarning: cannot stat '" << sub_path << "': " << std::strerror(errno) << "\n";
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            uint64_t child_inode = static_cast<uint64_t>(st.st_ino);

            // Mark this dir as seen
            seen_dir_inodes.insert(child_inode);

            auto existing_dir = db.get_dir(child_inode);
            if (!existing_dir.has_value()) {
                // New directory — add it
                db.add_dir({child_inode, dir_inode, entry->d_name, 0, 0, static_cast<int>(covered::DeltaState::New)});
                counts.dirs_new++;
            } else {
                // Clear any previous delta
                if (existing_dir->delta == static_cast<int>(covered::DeltaState::Deleted)) {
                    db.set_dir_delta(child_inode, 0);
                }
            }

            int sub_fd = openat(dir_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (sub_fd < 0) {
                std::cerr << "\nWarning: cannot open directory '" << sub_path << "': " << std::strerror(errno) << "\n";
                continue;
            }

            // Recurse into subdirectory
            update_dir(db, hash_db, sub_fd, child_inode, dir_inode, sub_path,
                       compute_hash, counts, seen_dir_inodes, seen_files_in_dir);
        } else if (S_ISREG(st.st_mode)) {
            uint64_t inode = static_cast<uint64_t>(st.st_ino);
            int64_t size = static_cast<int64_t>(st.st_size);
            int64_t mtime = static_cast<int64_t>(st.st_mtime);

            seen_files_in_dir[dir_inode].insert(entry->d_name);

            auto existing_file = db.get_file(dir_inode, entry->d_name);

            if (!existing_file.has_value()) {
                // New file — insert with delta='new'
                covered::FileEntry f;
                f.dir_inode = dir_inode;
                f.name = entry->d_name;
                f.inode = inode;
                f.size = size;
                f.mtime = mtime;
                f.covered = 0;
                f.error = 0;
                f.backup_id = 0;
                f.delta = static_cast<int>(covered::DeltaState::New);
                db.add_file(f);
                counts.files_new++;

                if (hash_db && compute_hash) {
                    compute_hashes_for_file(*hash_db, sub_path, size, inode);
                    counts.hashes_computed++;
                }
            } else {
                // File exists in DB — check if it changed
                bool changed = (existing_file->mtime != mtime || existing_file->size != size);

                if (changed) {
                    db.update_file(dir_inode, entry->d_name, inode, size, mtime);
                    counts.files_updated++;

                    // Unconditionally recompute hashes for changed files
                    if (hash_db) {
                        compute_hashes_for_file(*hash_db, sub_path, size, inode);
                        counts.hashes_computed++;
                    }
                } else {
                    // File unchanged — just clear any previous delta
                    if (existing_file->delta == static_cast<int>(covered::DeltaState::Deleted)) {
                        db.set_file_delta(dir_inode, entry->d_name, 0);
                    }
                    // Also update inode if it changed (e.g., file recreated with same name/size/mtime)
                    if (existing_file->inode != inode) {
                        db.update_file(dir_inode, entry->d_name, inode, size, mtime);
                    }
                }
            }
        }
    }

    closedir(dir);  // also closes dir_fd
    return true;
}

int cmd_update(int argc, char* argv[]) {
    bool compute_hash = false;

    // Parse args
    std::string coverdb_folder;
    std::string update_path;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--compute-hash") {
            compute_hash = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--compute-hash] <coverdb> <path>\n\n"
                      << "Recursively rescan a folder in an existing coverdb, updating the database\n"
                      << "with any changes (new files, deleted files, modified files).\n\n"
                      << "Options:\n"
                      << "  --compute-hash  Compute head and full blake3 hashes for new and changed files\n"
                      << "  --help, -h      Show this help\n";
            return 0;
        } else if (arg == "-f" || arg == "--force") {
            // Ignore -f flag silently for compatibility
        } else if (coverdb_folder.empty()) {
            coverdb_folder = arg;
        } else if (update_path.empty()) {
            update_path = arg;
        } else {
            std::cerr << "Too many arguments.\n";
            return 1;
        }
    }

    if (coverdb_folder.empty() || update_path.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--compute-hash] <coverdb> <path>\n";
        return 1;
    }

    // Normalize paths
    while (!coverdb_folder.empty() && coverdb_folder.back() == '/') {
        coverdb_folder.pop_back();
    }

    // Check that coverdb exists
    std::string db_path = coverdb_folder + "/filesize.db";
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "Error: database '" << db_path << "' does not exist.\n";
        return 1;
    }

    std::string config_path = coverdb_folder + "/config.json";
    if (!std::filesystem::exists(config_path)) {
        std::cerr << "Error: config.json not found in '" << coverdb_folder << "'.\n";
        return 1;
    }

    // Read root path from config.json
    std::string db_root;
    {
        std::ifstream f(config_path);
        if (!f) {
            std::cerr << "Error: cannot read config.json.\n";
            return 1;
        }
        try {
            nlohmann::json config = nlohmann::json::parse(f);
            if (!config.contains("root") || !config["root"].is_string()) {
                std::cerr << "Error: config.json does not contain 'root' string.\n";
                return 1;
            }
            db_root = config["root"].get<std::string>();
        } catch (const nlohmann::json::exception& e) {
            std::cerr << "Error: invalid config.json: " << e.what() << "\n";
            return 1;
        }
    }

    // Validate update_path is under db_root
    std::string abs_update_path = std::filesystem::absolute(update_path).string();

    // Normalize db_root for comparison
    std::string norm_db_root = db_root;
    while (!norm_db_root.empty() && norm_db_root.back() == '/') {
        norm_db_root.pop_back();
    }

    if (abs_update_path.compare(0, norm_db_root.size(), norm_db_root) != 0) {
        std::cerr << "Error: path '" << abs_update_path << "' is not within the DB filesystem root '" << norm_db_root << "'.\n";
        return 1;
    }

    // Open database
    covered::Database db(db_path);
    if (db.has_error()) {
        std::cerr << "Error opening database: " << db.error_msg() << "\n";
        return 1;
    }

    // Open hash database if needed
    std::string hash_db_path = coverdb_folder + "/hash.db";
    bool hash_db_exists = std::filesystem::exists(hash_db_path);
    std::unique_ptr<covered::HashDatabase> hash_db;

    if (compute_hash || hash_db_exists) {
        hash_db = std::make_unique<covered::HashDatabase>(hash_db_path);
        if (hash_db->has_error()) {
            std::cerr << "Error opening hash database: " << hash_db->error_msg() << "\n";
            return 1;
        }
    }

    // Find the directory inode for the path to update
    auto dir_inode_opt = db.find_dir_inode(abs_update_path);
    if (!dir_inode_opt.has_value()) {
        std::cerr << "Error: directory '" << abs_update_path << "' not found in database.\n";
        return 1;
    }
    uint64_t target_dir_inode = *dir_inode_opt;

    // Verify the directory exists on disk
    struct stat target_st;
    if (stat(abs_update_path.c_str(), &target_st) < 0 || !S_ISDIR(target_st.st_mode)) {
        std::cerr << "Error: '" << abs_update_path << "' is not a directory or does not exist.\n";
        return 1;
    }

    // Open the target directory
    int target_fd = open(abs_update_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (target_fd < 0) {
        std::cerr << "Error: cannot open '" << abs_update_path << "': " << std::strerror(errno) << "\n";
        return 1;
    }

    UpdateCounts counts;

    // Track which dirs/files in the DB we've seen on disk
    std::unordered_set<uint64_t> seen_dir_inodes;
    std::unordered_map<uint64_t, std::unordered_set<std::string>> seen_files_in_dir;

    // Mark the target directory itself as seen
    seen_dir_inodes.insert(target_dir_inode);
    // Clear any previous delta on the target dir
    auto target_dir = db.get_dir(target_dir_inode);
    if (target_dir.has_value() && target_dir->delta == static_cast<int>(covered::DeltaState::Deleted)) {
        db.set_dir_delta(target_dir_inode, 0);
    }

    db.begin_batch();

    // Recursively update
    auto parent_inode_opt = db.get_dir(target_dir_inode);
    uint64_t parent_inode = parent_inode_opt.has_value() ? parent_inode_opt->parent_inode : 0;

    bool ok = update_dir(db, hash_db.get(), target_fd, target_dir_inode, parent_inode,
                         abs_update_path, compute_hash, counts,
                         seen_dir_inodes, seen_files_in_dir);

    // Now find all DB entries under the subtree that were NOT seen on disk
    // and mark them as deleted
    auto subtree_dirs = db.get_dirs_in_subtree(target_dir_inode);
    for (const auto& d : subtree_dirs) {
        if (seen_dir_inodes.count(d.inode) == 0) {
            // Directory not seen on disk → mark as deleted (and all its files)
            db.mark_dir_deleted(d.inode);
            counts.dirs_deleted++;
        }
    }

    auto subtree_files = db.get_files_in_subtree(target_dir_inode);
    for (const auto& f : subtree_files) {
        auto it = seen_files_in_dir.find(f.dir_inode);
        if (it == seen_files_in_dir.end() || it->second.count(f.name) == 0) {
            // File in DB but not on disk → mark as deleted
            db.mark_file_deleted(f.dir_inode, f.name);
            counts.files_deleted++;
        }
    }

    db.commit_batch();

    if (hash_db) {
        hash_db->sync();
    }

    if (!ok) {
        std::cerr << "Error scanning directory.\n";
        return 1;
    }

    if (db.has_error()) {
        std::cerr << "Error during database updates: " << db.error_msg() << "\n";
        return 1;
    }

    // Print summary
    std::cout << "Update complete for '" << abs_update_path << "'\n";
    std::cout << "Files:   " << counts.files_new << " new, "
              << counts.files_deleted << " deleted, "
              << counts.files_updated << " updated\n";
    std::cout << "Dirs:    " << counts.dirs_new << " new, "
              << counts.dirs_deleted << " deleted\n";
    if (counts.hashes_computed > 0) {
        std::cout << "Hashes computed: " << counts.hashes_computed << "\n";
    }

    return 0;
}