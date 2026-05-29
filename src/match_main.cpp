#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstring>

#include <nlohmann/json.hpp>

#include "db.hpp"
#include "matcher.hpp"
#include "commands.hpp"

static std::string read_root_from_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    try {
        nlohmann::json config = nlohmann::json::parse(f);
        if (config.contains("root") && config["root"].is_string()) {
            return config["root"].get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // fall through
    }
    return "";
}

int cmd_match(int argc, char* argv[]) {
    bool debug = false;
    int arg_idx = 1;
    if (argc > 1 && std::string(argv[1]) == "-d") {
        debug = true;
        arg_idx = 2;
    }

    if (argc - arg_idx != 2) {
        std::cerr << "Usage: " << argv[0] << " [-d] <source_folder> <backup_folder>\n"
                  << "  e.g.: " << argv[0] << " -d covered_home_yves covered_mnt_backup\n";
        return 1;
    }

    std::string src_folder = argv[arg_idx];
    std::string bkp_folder = argv[arg_idx + 1];

    std::string src_db_path = src_folder + "/filesize.db";
    std::string bkp_db_path = bkp_folder + "/filesize.db";

    if (!std::filesystem::exists(src_db_path)) {
        std::cerr << "Error: source database not found: " << src_db_path << "\n";
        return 1;
    }
    if (!std::filesystem::exists(bkp_db_path)) {
        std::cerr << "Error: backup database not found: " << bkp_db_path << "\n";
        return 1;
    }

    // Database constructor auto-migrates error columns for backward compat
    covered::Database src_db(src_db_path);
    if (src_db.has_error()) {
        std::cerr << "Error opening source database: " << src_db.error_msg() << "\n";
        return 1;
    }
    covered::Database bkp_db(bkp_db_path);
    if (bkp_db.has_error()) {
        std::cerr << "Error opening backup database: " << bkp_db.error_msg() << "\n";
        return 1;
    }

    // Determine absolute backup root path for registration
    std::string bkp_abs_path = bkp_db.get_root_path().value_or("");
    if (bkp_abs_path.empty()) {
        bkp_abs_path = read_root_from_json(bkp_folder + "/config.json");
    }
    if (bkp_abs_path.empty()) {
        bkp_abs_path = std::filesystem::absolute(bkp_folder).string();
    }

    // Register this backup_db in the source DB's backup_db table.
    // Returns the id (existing or newly created).
    int backup_id = src_db.register_backup_db(bkp_abs_path);
    std::cout << "Backup registered with id=" << backup_id << " (path=" << bkp_abs_path << ")\n";

    std::string src_root = src_db.get_root_path().value_or("");
    std::string bkp_root = bkp_db.get_root_path().value_or("");

    if (src_root.empty()) {
        src_root = read_root_from_json(src_folder + "/config.json");
    }
    if (bkp_root.empty()) {
        bkp_root = read_root_from_json(bkp_folder + "/config.json");
    }

    if (src_root.empty()) {
        std::cerr << "Error: cannot determine source root path.\n";
        return 1;
    }
    if (bkp_root.empty()) {
        std::cerr << "Error: cannot determine backup root path.\n";
        return 1;
    }

    std::string src_hash_path = src_folder + "/hash.db";
    std::string bkp_hash_path = bkp_folder + "/hash.db";

    covered::HashDatabase src_hash(src_hash_path);
    if (src_hash.has_error()) {
        std::cerr << "Error opening source hash db: " << src_hash.error_msg() << "\n";
        return 1;
    }
    covered::HashDatabase bkp_hash(bkp_hash_path);
    if (bkp_hash.has_error()) {
        std::cerr << "Error opening backup hash db: " << bkp_hash.error_msg() << "\n";
        return 1;
    }

    std::cout << "Source root: " << src_root << "\n";
    std::cout << "Backup root: " << bkp_root << "\n";

    covered::Matcher matcher(src_db, src_hash, bkp_db, bkp_hash, src_root, bkp_root, backup_id, debug);
    bool ok = matcher.run();

    if (!ok) {
        std::cerr << "Error during matching.\n";
        return 1;
    }

    return 0;
}