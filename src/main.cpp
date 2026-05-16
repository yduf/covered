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

#include "db.hpp"
#include "scanner.hpp"

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
    return "covered_" + sanitized;
}

int main(int argc, char* argv[]) {
    bool force = false;
    std::string folder;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-f" || arg == "--force") {
            force = true;
        } else if (folder.empty()) {
            folder = arg;
        } else {
            std::cerr << "Usage: " << argv[0] << " [-f|--force] <folder>\n";
            return 1;
        }
    }

    if (folder.empty()) {
        std::cerr << "Usage: " << argv[0] << " [-f|--force] <folder>\n";
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

    covered::Database db(db_name);
    if (db.has_error()) {
        std::cerr << "Error opening database: " << db.error_msg() << "\n";
        return 1;
    }

    db.set_device(static_cast<uint64_t>(st.st_dev));

    // Store absolute root path for path reconstruction during match
    std::string abs_path = std::filesystem::absolute(folder).string();
    db.set_root_path(abs_path);

    // Also write a simple config.json for easy external access
    {
        std::ofstream cfg(db_folder + "/config.json");
        if (cfg) {
            cfg << "{\"root\":\"" << abs_path << "\"}\n";
        }
    }

    db.begin_batch();

    covered::Scanner scanner(db, static_cast<uint64_t>(st.st_dev));
    bool ok = scanner.scan(folder);

    db.commit_batch();

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

    return 0;
}
