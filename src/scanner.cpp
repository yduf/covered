#include "scanner.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <fstream>
#include <cerrno>

#include "blake3.h"

namespace covered {

static constexpr size_t HEAD_SIZE = 2048;
static constexpr size_t BLAKE3_HASH_LEN = BLAKE3_OUT_LEN; // 32 bytes

Scanner::Scanner(Database& db, uint64_t root_dev, HashDatabase* hash_db)
    : db_(db), hash_db_(hash_db), root_dev_(root_dev) {}

bool Scanner::scan(const std::string& root_path) {
    int fd = open(root_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    if (st.st_dev != static_cast<dev_t>(root_dev_)) {
        close(fd);
        return false;
    }

    // Extract basename for root directory entry
    std::string root_name = root_path;
    while (!root_name.empty() && root_name.back() == '/') {
        root_name.pop_back();
    }
    auto pos = root_name.find_last_of('/');
    if (pos != std::string::npos && pos + 1 < root_name.size()) {
        root_name = root_name.substr(pos + 1);
    }
    if (root_name.empty()) {
        root_name = "/";
    }

    db_.add_dir({static_cast<uint64_t>(st.st_ino), 0, root_name, 0, 0});
    dirs_seen_++;

    return scan_dir(fd, static_cast<uint64_t>(st.st_ino), root_path);
}

bool Scanner::scan_dir(int dir_fd, uint64_t dir_inode, const std::string& path) {
    DIR* dir = fdopendir(dir_fd);
    if (!dir) {
        close(dir_fd);
        // Could not open directory: mark the parent dir entry as having error
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string sub_path = path;
        if (!sub_path.empty() && sub_path.back() != '/') {
            sub_path += '/';
        }
        sub_path += entry->d_name;

        struct stat st;
        if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            std::cout << "\n" << std::flush;
            std::cerr << "Warning: cannot stat '" << sub_path << "': " << std::strerror(errno) << "\n";
            skipped_++;
            continue;
        }

        // Skip mount points (different device)
        if (st.st_dev != static_cast<dev_t>(root_dev_)) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int sub_fd = openat(dir_fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
            if (sub_fd < 0) {
                std::cout << "\n" << std::flush;
                std::cerr << "Warning: cannot open directory '" << sub_path << "': " << std::strerror(errno) << "\n";
                skipped_++;
                // Directory entry with error flag
                db_.add_dir({static_cast<uint64_t>(st.st_ino), dir_inode, entry->d_name, 0, 1});
                dirs_seen_++;
                continue;
            }

            db_.add_dir({static_cast<uint64_t>(st.st_ino), dir_inode, entry->d_name, 0, 0});
            dirs_seen_++;

            scan_dir(sub_fd, static_cast<uint64_t>(st.st_ino), sub_path);
        } else if (S_ISREG(st.st_mode)) {
            const char* fname = entry->d_name;
            uint64_t inode = static_cast<uint64_t>(st.st_ino);
            int64_t size = static_cast<int64_t>(st.st_size);

            // We cannot know yet if we have permission to read the file;
            // the error flag will be set during the match phase if reading fails.
            db_.add_file({
                dir_inode,
                fname,
                inode,
                size,
                static_cast<int64_t>(st.st_mtime),
                0,  // covered = 0 initially
                0   // error = 0 initially
            });

            // If hash computation is enabled, compute hashes immediately
            if (hash_db_) {
                compute_file_hashes(inode, sub_path, size);
            }

            auto count = ++files_seen_;
            if (count % 10000 == 0) {
                std::cout << "\rScanned " << count << " files so far... (current folder: " << path << ")" << std::flush;
            }
        }
        // Symlinks and special files are ignored
    }

    closedir(dir);  // also closes dir_fd
    return true;
}

void Scanner::compute_file_hashes(uint64_t inode, const std::string& path, int64_t size) {
    if (!hash_db_) return;

    // Skip empty files (head hash of empty file is not meaningful, full hash is deterministic but not useful)
    if (size == 0) return;

    // Check if we already have the hashes cached
    auto existing_head = hash_db_->get_head_hash(inode);
    auto existing_full = hash_db_->get_full_hash(inode);

    bool need_head = !existing_head.has_value();
    bool need_full = !existing_full.has_value();

    if (!need_head && !need_full) {
        return; // Both already cached
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "\nWarning: cannot open '" << path << "' for hash: " << std::strerror(errno) << "\n";
        db_.set_file_error(inode);
        return;
    }

    // Compute head hash if needed
    if (need_head) {
        char buffer[HEAD_SIZE];
        file.read(buffer, HEAD_SIZE);
        auto bytes_read = static_cast<size_t>(file.gcount());
        if (bytes_read > 0) {
            uint8_t head_hash[BLAKE3_HASH_LEN];
            blake3_hasher hasher_head;
            blake3_hasher_init(&hasher_head);
            blake3_hasher_update(&hasher_head, buffer, bytes_read);
            blake3_hasher_finalize(&hasher_head, head_hash, BLAKE3_HASH_LEN);
            hash_db_->set_head_hash(inode, head_hash, BLAKE3_HASH_LEN);
            head_hashes_computed_++;
        }
    }

    // Compute full hash if needed
    if (need_full) {
        // Reset to beginning of file to hash the entire content
        file.clear();
        file.seekg(0, std::ios::beg);
        if (!file) {
            std::cerr << "\nWarning: cannot re-read '" << path << "' for full hash: " << std::strerror(errno) << "\n";
            db_.set_file_error(inode);
            return;
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

        uint8_t full_hash[BLAKE3_HASH_LEN];
        blake3_hasher_finalize(&hasher, full_hash, BLAKE3_HASH_LEN);
        hash_db_->set_full_hash(inode, full_hash, BLAKE3_HASH_LEN);
        full_hashes_computed_++;
    }
}

} // namespace covered
