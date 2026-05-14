#include <iostream>
#include <string>
#include <filesystem>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

#include "db.hpp"
#include "scanner.hpp"

static std::string make_db_name(const std::string& path) {
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
    return "covered_" + sanitized + ".db";
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <folder>\n";
        return 1;
    }

    std::string folder = argv[1];

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

    std::string db_name = make_db_name(folder);

    // Abort if DB already exists
    if (std::filesystem::exists(db_name)) {
        std::cerr << "Error: database '" << db_name << "' already exists.\n";
        return 1;
    }

    covered::Database db(db_name);
    if (db.has_error()) {
        std::cerr << "Error opening database: " << db.error_msg() << "\n";
        return 1;
    }

    db.set_device(static_cast<uint64_t>(st.st_dev));
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
              << scanner.files_seen() << " files into " << db_name << "\n";

    return 0;
}
