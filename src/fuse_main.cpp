// FUSE filesystem exposing the covered arborescence with user.covered xattr.
// Usage: cover_fuse <source_folder> <mount_point>

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include <iostream>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "db.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

// ------------------------------------------------------------------
// In-memory filesystem node
// ------------------------------------------------------------------

enum class NodeKind { Dir, File };

struct FsNode {
    NodeKind    kind;
    int         covered;    // CoveredState: 0=uncovered,1=covered,2=partial,3=empty,4=error
    int         error;      // 1 if file/dir had an access error
    int64_t     size;       // files only
    int64_t     mtime;      // files only
    uint64_t    inode;      // files only: inode for hash lookup
    int         backup_id;  // files only: id of the backup_db that matched this file (0 = none)
    std::string real_path;  // files only: absolute path on real fs for read()
    std::vector<std::string> children; // basenames of direct children (dirs+files)
};

// ------------------------------------------------------------------
// Global filesystem state
// ------------------------------------------------------------------

class CoverFs {
public:
    std::string root_path;
    std::string src_db_folder; // path to the source DB folder (for hash.db)
    std::unordered_map<int, std::string> backup_paths; // backup_id -> absolute backup root path
    std::unordered_map<std::string, FsNode> nodes; // vpath -> FsNode
    covered::Database src_db; // the source filesize.db

    CoverFs(const std::string& src_folder)
        : src_db_folder(std::filesystem::absolute(src_folder).string())
        , src_db(src_folder + "/filesize.db")
    {
        std::string db_path = src_folder + "/filesize.db";
        if (!std::filesystem::exists(db_path)) {
            error_ = true;
            error_msg_ = "database not found: " + db_path;
            return;
        }
        if (src_db.has_error()) {
            error_ = true;
            error_msg_ = src_db.error_msg();
            return;
        }

        root_path = src_db.get_root_path().value_or("/");

        // Ensure dirs have covered column and error columns
        src_db.migrate_dirs_covered_column();
        src_db.migrate_error_columns();

        auto dirs = src_db.get_all_dirs();
        if (dirs.empty()) return;

        // Compute dir covered states
        auto dir_covered = src_db.compute_dir_covered();

        // Map inode -> dir entry for path building
        std::unordered_map<uint64_t, const covered::DirEntry*> by_inode;
        for (const auto& d : dirs) by_inode[d.inode] = &d;

        // Build path for each dir inode
        std::unordered_map<uint64_t, std::string> dir_paths;
        std::function<std::string(uint64_t)> get_dir_path = [&](uint64_t inode) -> std::string {
            auto it = dir_paths.find(inode);
            if (it != dir_paths.end()) return it->second;
            auto dit = by_inode.find(inode);
            if (dit == by_inode.end()) return "";
            const auto* d = dit->second;
            std::string path;
            if (d->parent_inode == 0) {
                path = "/"; // root
            } else {
                std::string parent = get_dir_path(d->parent_inode);
                path = (parent == "/" ? "" : parent) + "/" + d->name;
            }
            dir_paths[inode] = path;
            return path;
        };
        for (const auto& d : dirs) get_dir_path(d.inode);

        // Insert directory nodes
        for (const auto& d : dirs) {
            std::string vpath = dir_paths[d.inode];
            auto cit = dir_covered.find(d.inode);
            int cov_state = cit != dir_covered.end() ? cit->second : 0;

            FsNode node;
            node.kind      = NodeKind::Dir;
            node.covered   = cov_state;
            node.error     = d.error;
            node.size      = 0;
            node.mtime     = 0;
            node.inode     = 0;
            node.backup_id = 0;
            nodes[vpath] = node;
        }

        // Insert file nodes and register as children of their dir
        auto all_files = src_db.get_all_files();
        for (const auto& f : all_files) {
            auto pit = dir_paths.find(f.dir_inode);
            if (pit == dir_paths.end()) continue;
            const std::string& dir_vpath = pit->second;
            std::string vpath = (dir_vpath == "/" ? "" : dir_vpath) + "/" + f.name;

            FsNode node;
            node.kind      = NodeKind::File;
            node.covered   = f.covered ? static_cast<int>(covered::CoveredState::Covered)
                                       : (f.error ? static_cast<int>(covered::CoveredState::Error)
                                                  : static_cast<int>(covered::CoveredState::Uncovered));
            node.error     = f.error;
            node.size      = f.size;
            node.mtime     = f.mtime;
            node.inode     = f.inode;
            node.backup_id = f.backup_id;
            // Reconstruct real path from root_path
            node.real_path = root_path + vpath;
            nodes[vpath] = node;

            // Register as child of parent dir
            auto& dir_node = nodes[dir_vpath];
            dir_node.children.push_back(f.name);
        }

        for (const auto& d : dirs) {
            if (d.parent_inode == 0) continue;
            std::string parent_vpath = dir_paths[d.parent_inode];
            auto pit = nodes.find(parent_vpath);
            if (pit != nodes.end()) {
                pit->second.children.push_back(d.name);
            }
        }
        load_backup_paths();
    }

    bool empty() const { return nodes.empty(); }
    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

private:
    void load_backup_paths()
    {
        sqlite3* raw = src_db.raw_db();
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT id, path FROM backup_db";
        if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int id = sqlite3_column_int(stmt, 0);
                const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (p) backup_paths[id] = p;
            }
            sqlite3_finalize(stmt);
        }
    }

    bool error_ = false;
    std::string error_msg_;
};

// ------------------------------------------------------------------
// FUSE context helper
// ------------------------------------------------------------------

static CoverFs* get_fs()
{
    return static_cast<CoverFs*>(fuse_get_context()->private_data);
}

// ------------------------------------------------------------------
// Covered state → string for xattr
// ------------------------------------------------------------------

static const char* state_str(int state)
{
    switch (state) {
    case static_cast<int>(covered::CoveredState::Covered):   return "covered";
    case static_cast<int>(covered::CoveredState::Partial):   return "partial";
    case static_cast<int>(covered::CoveredState::Empty):     return "empty";
    case static_cast<int>(covered::CoveredState::Error):     return "error";
    default:                                                  return "uncovered";
    }
}

// ------------------------------------------------------------------
// Helper: build a path from inode using a filesize.db's dirs table
// ------------------------------------------------------------------

static std::string build_backup_path(uint64_t backup_inode, const std::string& bkp_db_folder)
{
    std::string db_path = bkp_db_folder + "/filesize.db";
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) return "";
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    std::vector<std::string> parts;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT parent_inode, name FROM dirs WHERE inode = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(backup_inode));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (name) parts.push_back(name);
                uint64_t parent = sqlite3_column_type(stmt, 0) == SQLITE_NULL
                                      ? 0
                                      : static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                sqlite3_finalize(stmt);
                stmt = nullptr;

                while (parent != 0) {
                    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(parent));
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                            if (n) parts.push_back(n);
                            parent = sqlite3_column_type(stmt, 0) == SQLITE_NULL
                                         ? 0
                                         : static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                        } else {
                            parent = 0;
                        }
                        sqlite3_finalize(stmt);
                        stmt = nullptr;
                    } else {
                        break;
                    }
                }
            }
        }
        if (stmt) sqlite3_finalize(stmt);
    }

    // Also get backup root path from config.json
    std::string root_path;
    {
        std::string config_path = bkp_db_folder + "/config.json";
        std::ifstream f(config_path);
        if (f) {
            try {
                nlohmann::json config = nlohmann::json::parse(f);
                if (config.contains("root") && config["root"].is_string()) {
                    root_path = config["root"].get<std::string>();
                }
            } catch (const nlohmann::json::exception&) {
                // fall through
            }
        }
    }
    sqlite3_close(db);

    if (parts.empty()) return "";

    std::string result = root_path;
    if (!result.empty() && result.back() != '/') result += '/';
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        result += *it;
        if (it + 1 != parts.rend()) result += '/';
    }
    return result;
}

// ------------------------------------------------------------------
// Helper: derive the DB folder path from backup root path
// ------------------------------------------------------------------

static std::string derive_backup_db_folder(const std::string& bkp_root_path, const std::string& src_db_folder)
{
    std::string sanitized = bkp_root_path;
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
    std::string db_folder_name = "covered_" + sanitized;

    // Get sibling of source DB folder
    std::filesystem::path src_path(src_db_folder);
    std::filesystem::path parent = src_path.parent_path();
    return (parent / db_folder_name).string();
}

// ------------------------------------------------------------------
// Helper: find backup file path from source inode using hash lookup
// ------------------------------------------------------------------

static std::string resolve_covered_at(CoverFs* fs, uint64_t src_inode, int backup_id)
{
    // 1. Get source file's full_hash from source hash.db
    std::string src_hash_path = fs->src_db_folder + "/hash.db";
    std::optional<std::vector<uint8_t>> src_full_hash;

    {
        sqlite3* hdb = nullptr;
        if (sqlite3_open(src_hash_path.c_str(), &hdb) != SQLITE_OK) return "";
        sqlite3_exec(hdb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT full_hash FROM hashes WHERE inode = ?";
        if (sqlite3_prepare_v2(hdb, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(src_inode));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const void* blob = sqlite3_column_blob(stmt, 0);
                int len = sqlite3_column_bytes(stmt, 0);
                if (blob && len > 0) {
                    src_full_hash = std::vector<uint8_t>(
                        static_cast<const uint8_t*>(blob),
                        static_cast<const uint8_t*>(blob) + len);
                }
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(hdb);
    }

    if (!src_full_hash.has_value() || src_full_hash->empty()) {
        fprintf(stderr, "resolve_covered_at: no full_hash for src_inode=%lu\n", (unsigned long)src_inode);
        return "";
    }

    // 2. Get backup db_folder path derived on-the-fly
    auto it_path = fs->backup_paths.find(backup_id);
    if (it_path == fs->backup_paths.end()) {
        fprintf(stderr, "resolve_covered_at: backup_paths not found for id=%d\n", backup_id);
        return "";
    }
    std::string bkp_root = it_path->second;
    std::string bkp_db_folder = derive_backup_db_folder(bkp_root, fs->src_db_folder);
    fprintf(stderr, "resolve_covered_at: bkp_root='%s' bkp_db_folder='%s'\n", bkp_root.c_str(), bkp_db_folder.c_str());

    // 3. Find matching inode in backup hash.db via indexed full_hash
    std::string bkp_hash_path = bkp_db_folder + "/hash.db";
    uint64_t bkp_inode = 0;
    fprintf(stderr, "resolve_covered_at: opening bkp_hash_path='%s'\n", bkp_hash_path.c_str());

    {
        sqlite3* bdb = nullptr;
        if (sqlite3_open(bkp_hash_path.c_str(), &bdb) != SQLITE_OK) {
            fprintf(stderr, "resolve_covered_at: failed to open bkp hash.db\n");
            return "";
        }
        sqlite3_exec(bdb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT inode FROM hashes WHERE full_hash = ? LIMIT 1";
        if (sqlite3_prepare_v2(bdb, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_blob(stmt, 1, src_full_hash->data(),
                              static_cast<int>(src_full_hash->size()), SQLITE_STATIC);
            int rc = sqlite3_step(stmt);
            fprintf(stderr, "resolve_covered_at: hash lookup rc=%d\n", rc);
            if (rc == SQLITE_ROW) {
                bkp_inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                fprintf(stderr, "resolve_covered_at: found bkp_inode=%lu\n", (unsigned long)bkp_inode);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(bdb);
    }

    if (bkp_inode == 0) {
        fprintf(stderr, "resolve_covered_at: bkp_inode not found\n");
        return "";
    }

    // 4. Build full path from backup filesize.db
    std::string result = build_backup_path(bkp_inode, bkp_db_folder);
    fprintf(stderr, "resolve_covered_at: result='%s'\n", result.c_str());
    return result;
}

// ------------------------------------------------------------------
// FUSE operations
// ------------------------------------------------------------------

static int cfs_getattr(const char* path, struct stat* st, struct fuse_file_info* /*fi*/)
{
    memset(st, 0, sizeof(*st));
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;
    const FsNode& node = it->second;
    if (node.kind == NodeKind::Dir) {
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_mtime = 0;
    } else {
        st->st_mode  = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size  = node.size;
        st->st_mtime = static_cast<time_t>(node.mtime);
    }
    return 0;
}

static int cfs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t /*offset*/, struct fuse_file_info* /*fi*/,
                       enum fuse_readdir_flags /*flags*/)
{
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;
    if (it->second.kind != NodeKind::Dir) return -ENOTDIR;
    filler(buf, ".",  nullptr, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", nullptr, 0, FUSE_FILL_DIR_PLUS);
    for (const auto& child_name : it->second.children) {
        filler(buf, child_name.c_str(), nullptr, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

static int cfs_open(const char* path, struct fuse_file_info* fi)
{
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;
    if (it->second.kind != NodeKind::File) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    if (it->second.error) return -EACCES;
    return 0;
}

static int cfs_read(const char* path, char* buf, size_t size, off_t offset,
                    struct fuse_file_info* /*fi*/)
{
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;
    if (it->second.kind != NodeKind::File) return -EISDIR;
    const FsNode& node = it->second;
    if (node.error || node.real_path.empty()) return -EACCES;
    int fd = ::open(node.real_path.c_str(), O_RDONLY);
    if (fd < 0) return -errno;
    int res = static_cast<int>(::pread(fd, buf, size, offset));
    if (res < 0) res = -errno;
    ::close(fd);
    return res;
}

// ------------------------------------------------------------------
// Extended attributes
// ------------------------------------------------------------------

static std::string compact_path(const std::string& path)
{
    if (path.empty()) return "";
    std::vector<std::string> parts;
    size_t start = (path[0] == '/') ? 1 : 0;
    std::string current;
    for (size_t i = start; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += path[i];
        }
    }
    if (parts.empty()) return "";
    std::string result = "/";
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        result += parts[i][0];
        result += "/";
    }
    result += parts.back();
    return result;
}

static int cfs_listxattr(const char* path, char* buf, size_t size)
{
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;

    const FsNode& node = it->second;
    size_t needed = sizeof("user.covered");
    bool add_extra = false;
    if (node.kind == NodeKind::File
        && node.covered == static_cast<int>(covered::CoveredState::Covered)
        && node.backup_id > 0) {
        needed += sizeof("user.covered_backup") + sizeof("user.covered_at");
        add_extra = true;
    }
    if (size == 0) return static_cast<int>(needed);
    if (size < needed) return -ERANGE;

    char* p = buf;
    memcpy(p, "user.covered", sizeof("user.covered"));
    p += sizeof("user.covered");
    if (add_extra) {
        memcpy(p, "user.covered_backup", sizeof("user.covered_backup"));
        p += sizeof("user.covered_backup");
        memcpy(p, "user.covered_at", sizeof("user.covered_at"));
        p += sizeof("user.covered_at");
    }
    return static_cast<int>(needed);
}

static int cfs_getxattr(const char* path, const char* name, char* buf, size_t size)
{
    CoverFs* fs = get_fs();
    auto it = fs->nodes.find(path);
    if (it == fs->nodes.end()) return -ENOENT;

    const FsNode& node = it->second;

    // user.covered — the standard coverage state
    if (strcmp(name, "user.covered") == 0) {
        const char* val = state_str(node.covered);
        size_t vlen = strlen(val);
        if (size == 0) return static_cast<int>(vlen);
        if (size < vlen) return -ERANGE;
        memcpy(buf, val, vlen);
        return static_cast<int>(vlen);
    }

    // user.covered_backup — compact form of the backup DB root path
    if (strcmp(name, "user.covered_backup") == 0) {
        if (node.backup_id <= 0) return -ENODATA;
        auto bit = fs->backup_paths.find(node.backup_id);
        if (bit == fs->backup_paths.end()) return -ENODATA;
        std::string compact = compact_path(bit->second);
        size_t vlen = compact.size();
        if (size == 0) return static_cast<int>(vlen);
        if (size < vlen) return -ERANGE;
        memcpy(buf, compact.c_str(), vlen);
        return static_cast<int>(vlen);
    }

    // user.covered_at — full path of the file in the backup
    // Format: backup_root + relative_path
    // The relative path is the same as in the source filesystem (same tree structure)
    if (strcmp(name, "user.covered_at") == 0) {
        if (node.backup_id <= 0 || node.inode == 0) return -ENODATA;
        std::string full = resolve_covered_at(fs, node.inode, node.backup_id);
        if (full.empty()) return -ENODATA;
        size_t vlen = full.size();
        if (size == 0) return static_cast<int>(vlen);
        if (size < vlen) return -ERANGE;
        memcpy(buf, full.c_str(), vlen);
        return static_cast<int>(vlen);
    }

    return -ENODATA;
}

// ------------------------------------------------------------------
// main
// ------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // We expect: cover_fuse <source_folder> <mount_point> [fuse options]
    // We strip our two args and pass the rest to fuse_main.
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <source_folder> <mount_point> [fuse options]\n";
        return 1;
    }

    std::string src_folder  = argv[1];
    std::string mount_point = argv[2];

    CoverFs fs(src_folder);
    if (fs.has_error()) {
        std::cerr << "Error opening database: " << fs.error_msg() << "\n";
        return 1;
    }
    if (fs.empty()) {
        std::cerr << "Error: failed to build filesystem from database.\n";
        return 1;
    }

    std::cerr << "Mounted " << fs.nodes.size() << " entries from " << src_folder
              << " at " << mount_point << "\n";

    std::vector<std::string> fuse_args_str;
    fuse_args_str.push_back(argv[0]);
    fuse_args_str.push_back(mount_point);
    for (int i = 3; i < argc; ++i) fuse_args_str.push_back(argv[i]);
    bool has_fg = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "-f" || std::string(argv[i]) == "-d") has_fg = true;
    if (!has_fg) fuse_args_str.push_back("-f");

    std::vector<char*> fuse_argv;
    for (auto& s : fuse_args_str) fuse_argv.push_back(const_cast<char*>(s.c_str()));

    struct fuse_operations ops;
    memset(&ops, 0, sizeof(ops));
    ops.getattr   = cfs_getattr;
    ops.readdir   = cfs_readdir;
    ops.open      = cfs_open;
    ops.read      = cfs_read;
    ops.getxattr  = cfs_getxattr;
    ops.listxattr = cfs_listxattr;

    return fuse_main(static_cast<int>(fuse_argv.size()), fuse_argv.data(), &ops, &fs);
}