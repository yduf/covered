#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include <nlohmann/json.hpp>

#include "db.hpp"
#include "scanner.hpp"
#include "blake3.h"

static std::string make_db_folder(const std::string& path) {
    std::string sanitized = path;
    // Remove trailing slashes
    while (!sanitized.empty() && sanitized.back() == '/') {
        sanitized.pop_back();
    }
    // Remove leading slash
    if (!sanitized.empty() && sanitized.front() == '/') {
        sanitized = sanitized.substr(1);
    }
    // Replace slashes with underscores
    for (char& c : sanitized) {
        if (c == '/') {
            c = '_';
        }
    }
    if (sanitized.empty()) {
        sanitized = "root";
    }
    return "coverdb/" + sanitized;
}

#include "commands.hpp"

int cmd_scan(int argc, char* argv[]) {
    bool force = false;
    bool compute_hash = false;
    std::string folder;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--force") {
            force = true;
        } else if (arg == "--compute-hash") {
            compute_hash = true;
        } else if (folder.empty()) {
            folder = arg;
        } else {
            std::cerr << "Usage: " << argv[0] << " [-f|--force] [--compute-hash] <folder>\n";
            return 1;
        }
    }

    if (folder.empty()) {
        std::cerr << "Usage: " << argv[0] << " [-f|--force] [--compute-hash] <folder>\n";
        return 1;
    }

    // Verify target is a directory
    struct stat st;
    if (stat(folder.c_str(), &st) < 0) {
        std::cerr << "Error: cannot stat '" << folder << "': " << std::strerror(errno) << "\n";
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        std::cerr << "Error: '" << folder << "' is not a directory.\n";
        return 1;
    }

    std::string db_folder = make_db_folder(folder);
    std::string db_name = db_folder + "/filesize.db";

    // Create output folder if needed
    if (!std::filesystem::exists(db_folder)) {
        std::filesystem::create_directories(db_folder);
    }

    // Handle existing DB
    if (std::filesystem::exists(db_name)) {
        if (!force) {
            std::cerr << "Error: database '" << db_name << "' already exists. Use -f to overwrite.\n";
            return 1;
        }
        std::filesystem::remove(db_name);
        std::filesystem::remove(db_name + "-shm");
        std::filesystem::remove(db_name + "-wal");
    }

    // Handle hash DB when --compute-hash is used
    std::string hash_db_path = db_folder + "/hash.db";
    std::unique_ptr<covered::HashDatabase> hash_db;

    if (compute_hash) {
        if (std::filesystem::exists(hash_db_path)) {
            if (!force) {
                std::cerr << "Error: hash database '" << hash_db_path << "' already exists. Use -f to overwrite.\n";
                return 1;
            }
            std::filesystem::remove(hash_db_path);
            std::filesystem::remove(hash_db_path + "-shm");
            std::filesystem::remove(hash_db_path + "-wal");
        }

        hash_db = std::make_unique<covered::HashDatabase>(hash_db_path);
        if (hash_db->has_error()) {
            std::cerr << "Error opening hash database: " << hash_db->error_msg() << "\n";
            return 1;
        }
    }

    covered::Database db(db_name);
    if (db.has_error()) {
        std::cerr << "Error opening database: " << db.error_msg() << "\n";
        return 1;
    }

    // Store absolute root path for path reconstruction during match
    std::string abs_path = std::filesystem::absolute(folder).string();

    // Write config.json using nlohmann::json
    {
        nlohmann::json config;
        config["root"] = abs_path;
        config["device"] = static_cast<uint64_t>(st.st_dev);
        std::ofstream cfg(db_folder + "/config.json");
        if (cfg) {
            cfg << config.dump() << "\n";
        }
    }

    db.begin_batch();

    covered::Scanner scanner(db, static_cast<uint64_t>(st.st_dev), hash_db.get());
    bool ok = scanner.scan(folder);

    db.commit_batch();

    if (hash_db) {
        hash_db->sync();
    }

    if (!ok) {
        std::cerr << "Error scanning directory.\n";
        return 1;
    }

    if (db.has_error()) {
        std::cerr << "Error during database writes: " << db.error_msg() << "\n";
        return 1;
    }

    if (scanner.files_seen() > 0) {
        std::cout << "\n";
    }
    std::cout << "Scanned " << scanner.dirs_seen() << " directories and "
              << scanner.files_seen() << " files into " << db_name;
    if (scanner.skipped() > 0) {
        std::cout << " (" << scanner.skipped() << " skipped)";
    }
    std::cout << "\n";

    if (compute_hash) {
        std::cout << "Head hashes computed: " << scanner.head_hashes_computed() << "\n";
        std::cout << "Full hashes computed: " << scanner.full_hashes_computed() << "\n";
    }

    return 0;
}
