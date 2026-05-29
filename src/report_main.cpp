#include <iostream>
#include <string>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "db.hpp"
#include "commands.hpp"

// -------------------------------------------------------------------
// main
// -------------------------------------------------------------------
int cmd_report(int argc, char* argv[])
{
    bool do_report = false;
    std::string src_folder;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--report" || arg == "-r") {
            do_report = true;
        } else if (src_folder.empty()) {
            src_folder = arg;
        } else {
            std::cerr << "Usage: " << argv[0] << " [--report|-r] <source_folder>\n";
            return 1;
        }
    }

    if (src_folder.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--report|-r] <source_folder>\n";
        return 1;
    }

    std::string db_path = src_folder + "/filesize.db";
    if (!std::filesystem::exists(db_path)) {
        std::cerr << "Error: database not found: " << db_path << "\n";
        return 1;
    }

    covered::Database db(db_path);
    if (db.has_error()) {
        std::cerr << "Error opening database: " << db.error_msg() << "\n";
        return 1;
    }

    // Step 1: migrate schema – add covered column to dirs and error columns if missing
    db.migrate_dirs_covered_column();
    db.migrate_error_columns();

    // Step 2: get root path
    std::string root_path = db.get_root_path().value_or(src_folder);

    // Step 3: load all dirs and compute covered state bottom-up
    auto dirs = db.get_all_dirs();
    if (dirs.empty()) {
        std::cerr << "Warning: no directories found in database.\n";
    }

    auto dir_covered = db.compute_dir_covered();

    // Step 4: write back covered states to dirs table (inside a transaction)
    {
        sqlite3* raw = db.raw_db();
        char* errmsg = nullptr;
        sqlite3_exec(raw, "BEGIN;", nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg);

        for (const auto& [inode, state] : dir_covered) {
            db.set_dir_covered(inode, state);
        }

        errmsg = nullptr;
        sqlite3_exec(raw, "COMMIT;", nullptr, nullptr, &errmsg);
        if (errmsg) sqlite3_free(errmsg);
    }

    db.sync();

    // Step 5: stats
    auto all_files = db.get_all_files();
    uint64_t total_files    = all_files.size();
    uint64_t covered_files  = 0;
    uint64_t error_files    = 0;
    for (const auto& f : all_files) {
        if (f.covered) ++covered_files;
        if (f.error)   ++error_files;
    }
    uint64_t uncovered_files = total_files - covered_files;

    uint64_t total_dirs     = dirs.size();
    uint64_t covered_dirs   = 0;
    uint64_t partial_dirs   = 0;
    uint64_t uncovered_dirs = 0;
    uint64_t empty_dirs     = 0;
    uint64_t error_dirs     = 0;
    for (const auto& [inode, state] : dir_covered) {
        if      (state == static_cast<int>(covered::CoveredState::Covered))   ++covered_dirs;
        else if (state == static_cast<int>(covered::CoveredState::Partial))   ++partial_dirs;
        else if (state == static_cast<int>(covered::CoveredState::Empty))     ++empty_dirs;
        else if (state == static_cast<int>(covered::CoveredState::Error))     ++error_dirs;
        else                                                                   ++uncovered_dirs;
    }

    std::cout << "Source: " << root_path << "\n";
    std::cout << "Files   : " << total_files
              << "  covered=" << covered_files
              << "  uncovered=" << uncovered_files
              << "  error=" << error_files << "\n";
    std::cout << "Dirs    : " << total_dirs
              << "  covered=" << covered_dirs
              << "  partial=" << partial_dirs
              << "  uncovered=" << uncovered_dirs
              << "  empty=" << empty_dirs
              << "  error=" << error_dirs << "\n";

    // Step 6: optional report of uncovered files
    if (do_report && uncovered_files > 0) {
        std::cout << "\nUncovered files:\n";

        // Build path map
        auto dir_paths = db.build_dir_paths(root_path);

        for (const auto& f : all_files) {
            if (!f.covered) {
                auto it = dir_paths.find(f.dir_inode);
                std::string dir_path = (it != dir_paths.end()) ? it->second : "?";
                std::cout << dir_path << "/" << f.name;
                if (f.error) std::cout << " [ERROR]";
                std::cout << "\n";
            }
        }
    }

    return 0;
}