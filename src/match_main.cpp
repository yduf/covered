#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstring>

#include "db.hpp"
#include "matcher.hpp"

static std::string read_root_from_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::string line;
    if (std::getline(f, line)) {
        auto pos = line.find("\"root\":\"");
        if (pos != std::string::npos) {
            pos += 8;
            auto end = line.find("\"", pos);
            if (end != std::string::npos) {
                return line.substr(pos, end - pos);
            }
        }
    }
    return "";
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <source_folder> <backup_folder>\n"
                  << "  e.g.: " << argv[0] << " covered_home_yves covered_mnt_backup\n";
        return 1;
    }

    std::string src_folder = argv[1];
    std::string bkp_folder = argv[2];

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

    covered::Matcher matcher(src_db, src_hash, bkp_db, bkp_hash, src_root, bkp_root);
    bool ok = matcher.run();

    if (!ok) {
        std::cerr << "Error during matching.\n";
        return 1;
    }

    return 0;
}
