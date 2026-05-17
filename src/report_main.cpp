#include <iostream>
#include <string>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "db.hpp"

// -------------------------------------------------------------------
// Path reconstruction helper
// Build a map inode -> full path from the dirs table
// -------------------------------------------------------------------
static std::unordered_map<uint64_t, std::string>
build_dir_paths(const std::vector<covered::DirEntry>& dirs, const std::string& root_path)
{
    // Map inode -> DirEntry for quick lookup
    std::unordered_map<uint64_t, const covered::DirEntry*> by_inode;
    for (const auto& d : dirs) {
        by_inode[d.inode] = &d;
    }

    std::unordered_map<uint64_t, std::string> paths;

    // Recursive lambda (iterative via memoization)
    std::function<std::string(uint64_t)> get_path = [&](uint64_t inode) -> std::string {
        auto it = paths.find(inode);
        if (it != paths.end()) return it->second;

        auto dit = by_inode.find(inode);
        if (dit == by_inode.end()) return root_path; // fallback

        const auto* d = dit->second;
        if (d->parent_inode == 0) {
            // root directory
            paths[inode] = root_path;
        } else {
            paths[inode] = get_path(d->parent_inode) + "/" + d->name;
        }
        return paths[inode];
    };

    for (const auto& d : dirs) {
        get_path(d.inode);
    }
    return paths;
}

// -------------------------------------------------------------------
// Compute covered state for all directories (bottom-up)
// Returns a map inode -> CoveredState (0=uncovered,1=covered,2=partial)
// -------------------------------------------------------------------
static std::unordered_map<uint64_t, int>
compute_dir_covered(covered::Database& db,
                    const std::vector<covered::DirEntry>& dirs)
{
    // Build children map
    std::unordered_map<uint64_t, std::vector<uint64_t>> children; // parent_inode -> child inodes
    uint64_t root_inode = 0;
    for (const auto& d : dirs) {
        if (d.parent_inode == 0) {
            root_inode = d.inode;
        } else {
            children[d.parent_inode].push_back(d.inode);
        }
    }

    std::unordered_map<uint64_t, int> result;

    // Post-order DFS (iterative with explicit stack)
    // We need to process children before parents
    std::vector<uint64_t> order;
    {
        std::vector<uint64_t> stack;
        if (root_inode != 0) stack.push_back(root_inode);
        // Also push any dirs that might be disconnected
        for (const auto& d : dirs) {
            if (d.parent_inode == 0 && d.inode != root_inode) stack.push_back(d.inode);
        }
        while (!stack.empty()) {
            uint64_t cur = stack.back();
            stack.pop_back();
            order.push_back(cur);
            auto cit = children.find(cur);
            if (cit != children.end()) {
                for (auto child : cit->second) {
                    stack.push_back(child);
                }
            }
        }
        // Process in reverse so leaves come first
        std::reverse(order.begin(), order.end());
    }

    for (uint64_t inode : order) {
        // Get files directly in this dir
        auto files = db.get_files_by_dir(inode);

        int total = static_cast<int>(files.size());
        int cov   = 0;
        for (const auto& f : files) {
            if (f.covered) ++cov;
        }

        // Aggregate children dirs
        auto cit = children.find(inode);
        if (cit != children.end()) {
            for (uint64_t child_inode : cit->second) {
                auto rit = result.find(child_inode);
                if (rit != result.end()) {
                    int child_state = rit->second;
                    if (child_state == static_cast<int>(covered::CoveredState::Covered)) {
                        // Fully covered child: one virtual covered item
                        ++total;
                        ++cov;
                    } else if (child_state == static_cast<int>(covered::CoveredState::Partial)) {
                        // Partial child forces parent to be partial:
                        // add 2 virtual items (1 covered + 1 uncovered)
                        total += 2;
                        cov   += 1;
                    } else {
                        // Uncovered child: one virtual uncovered item
                        ++total;
                    }
                }
            }
        }

        int state;
        if (total == 0) {
            state = static_cast<int>(covered::CoveredState::Covered); // empty dir = covered
        } else if (cov == 0) {
            state = static_cast<int>(covered::CoveredState::Uncovered);
        } else if (cov >= total) {
            state = static_cast<int>(covered::CoveredState::Covered);
        } else {
            state = static_cast<int>(covered::CoveredState::Partial);
        }
        result[inode] = state;
    }

    return result;
}

// -------------------------------------------------------------------
// main
// -------------------------------------------------------------------
int main(int argc, char* argv[])
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

    // Step 1: migrate schema – add covered column to dirs if missing
    db.migrate_dirs_covered_column();

    // Step 2: get root path
    std::string root_path = db.get_root_path().value_or(src_folder);

    // Step 3: load all dirs and compute covered state bottom-up
    auto dirs = db.get_all_dirs();
    if (dirs.empty()) {
        std::cerr << "Warning: no directories found in database.\n";
    }

    auto dir_covered = compute_dir_covered(db, dirs);

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
    for (const auto& f : all_files) {
        if (f.covered) ++covered_files;
    }
    uint64_t uncovered_files = total_files - covered_files;

    uint64_t total_dirs    = dirs.size();
    uint64_t covered_dirs  = 0;
    uint64_t partial_dirs  = 0;
    uint64_t uncovered_dirs = 0;
    for (const auto& [inode, state] : dir_covered) {
        if (state == static_cast<int>(covered::CoveredState::Covered))   ++covered_dirs;
        else if (state == static_cast<int>(covered::CoveredState::Partial))  ++partial_dirs;
        else ++uncovered_dirs;
    }

    std::cout << "Source: " << root_path << "\n";
    std::cout << "Files   : " << total_files
              << "  covered=" << covered_files
              << "  uncovered=" << uncovered_files << "\n";
    std::cout << "Dirs    : " << total_dirs
              << "  covered=" << covered_dirs
              << "  partial=" << partial_dirs
              << "  uncovered=" << uncovered_dirs << "\n";

    // Step 6: optional report of uncovered files
    if (do_report && uncovered_files > 0) {
        std::cout << "\nUncovered files:\n";

        // Build path map
        auto dir_paths = build_dir_paths(dirs, root_path);

        for (const auto& f : all_files) {
            if (!f.covered) {
                auto it = dir_paths.find(f.dir_inode);
                std::string dir_path = (it != dir_paths.end()) ? it->second : "?";
                std::cout << dir_path << "/" << f.name << "\n";
            }
        }
    }

    return 0;
}
