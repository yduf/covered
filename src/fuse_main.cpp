// FUSE filesystem exposing the covered arborescence with user.covered xattr.
// Usage: cover_fuse <source_folder> <mount_point>

#define FUSE_USE_VERSION 31
#include <fuse3/fuse_lowlevel.h>

#include <iostream>
#include <string>
#include <filesystem>
#include <unordered_map>
#include <vector>
#include <algorithm>
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
    uint64_t    db_inode;   // original inode from DB (for hash lookups)
    fuse_ino_t  parent_ino; // parent directory fuse inode
    int         backup_id;  // files only: id of the backup_db that matched this file (0 = none)
    std::string real_path;  // files only: absolute path on real fs for read()
    std::string name;       // basename
};

// Forward declaration for helper used in CoverFs constructor
static std::string derive_backup_db_folder(const std::string& bkp_root_path, const std::string& src_db_folder);

// ------------------------------------------------------------------
// Filesystem state — inode-indexed (low-level FUSE ready)
// ------------------------------------------------------------------

class CoverFs {
public:
    covered::Database src_db; // the source filesize.db
    std::unordered_map<int, covered::HashDatabase> bck_db; // backup_id -> backup

    CoverFs(const std::string& src_folder) :
        src_db(std::filesystem::absolute(src_folder).string() + "/filesize.db"),
        src_folder_(std::filesystem::absolute(src_folder).string())
    {
        src_db.migrate_dirs_covered_column();
        src_db.migrate_error_columns();

        {
            for (const auto& [id, path] : src_db.get_backup_paths()) {
                std::string bkp_db_folder = derive_backup_db_folder(path, src_folder_);
                std::string hash_path = bkp_db_folder + "/hash.db";
                bck_db.emplace(id, hash_path);
            }
        }

        auto dirs = src_db.get_all_dirs();
        if (dirs.empty()) return;

        auto dir_covered = src_db.compute_dir_covered();

        // Find root dir (parent_inode == 0) and map it to FUSE_ROOT_ID
        for (const auto& d : dirs) {
            if (d.parent_inode == 0) {
                root_db_ino_ = d.inode;
                break;
            }
        }

        // Pre-compute max inode for collision avoidance with FUSE_ROOT_ID
        uint64_t max_ino = FUSE_ROOT_ID;
        for (const auto& d : dirs) {
            if (d.inode > max_ino) max_ino = d.inode;
        }
        auto all_files = src_db.get_all_files();
        for (const auto& f : all_files) {
            if (f.inode > max_ino) max_ino = f.inode;
        }
        next_gen_ino_ = max_ino + 1;

        auto root_path = src_db.get_root_path().value_or("/");

        // Insert directory nodes
        for (const auto& d : dirs) {
            fuse_ino_t ino = map_db_inode(d.inode);
            fuse_ino_t parent_ino = (d.parent_inode == 0) ? 0 : map_db_inode(d.parent_inode);

            auto cit = dir_covered.find(d.inode);
            int cov_state = cit != dir_covered.end() ? cit->second : 0;

            FsNode node;
            node.kind      = NodeKind::Dir;
            node.covered   = cov_state;
            node.error     = d.error;
            node.size      = 0;
            node.mtime     = 0;
            node.db_inode  = d.inode;
            node.parent_ino = parent_ino;
            node.backup_id = 0;
            node.real_path = ""; // directories have no real path
            node.name      = d.name;

            inodes_[ino] = node;

            if (parent_ino != 0 || d.parent_inode != 0) {
                children_[parent_ino].push_back({d.name, ino});
            }
        }

        // Insert file nodes
        for (const auto& f : all_files) {
            fuse_ino_t ino = map_db_inode(f.inode);
            fuse_ino_t parent_ino = map_db_inode(f.dir_inode);

            // Build real path: root_path + relative path
            // We still need the vpath for real_path, but only for files
            std::string rel_path = build_relative_path(parent_ino, f.name);
            std::string real_path = root_path + rel_path;

            FsNode node;
            node.kind      = NodeKind::File;
            node.covered   = f.covered ? static_cast<int>(covered::CoveredState::Covered)
                                       : (f.error ? static_cast<int>(covered::CoveredState::Error)
                                                  : static_cast<int>(covered::CoveredState::Uncovered));
            node.error     = f.error;
            node.size      = f.size;
            node.mtime     = f.mtime;
            node.db_inode  = f.inode;
            node.parent_ino = parent_ino;
            node.backup_id = f.backup_id;
            node.real_path = real_path;
            node.name      = f.name;

            inodes_[ino] = node;
            children_[parent_ino].push_back({f.name, ino});
        }
    }

    bool empty() const { return inodes_.empty(); }
    bool has_error() const { return error_; }
    const std::string& error_msg() const { return error_msg_; }

    // Lookup by inode (thread-safe — read-only after construction)
    const FsNode* lookup_ino(fuse_ino_t ino) const
    {
        auto it = inodes_.find(ino);
        return (it != inodes_.end()) ? &it->second : nullptr;
    }

    // Lookup child by name within a directory
    fuse_ino_t lookup_child(fuse_ino_t parent, const std::string& name) const
    {
        auto it = children_.find(parent);
        if (it == children_.end()) return 0;
        for (const auto& [child_name, child_ino] : it->second) {
            if (child_name == name) return child_ino;
        }
        return 0;
    }

    // Read directory entries — returns children for ino
    const std::vector<std::pair<std::string, fuse_ino_t>>* read_dir(fuse_ino_t ino) const
    {
        auto it = children_.find(ino);
        return (it != children_.end()) ? &it->second : nullptr;
    }

    // Node count for diagnostics
    size_t node_count() const { return inodes_.size(); }

    // Resolve covered_at path from inode
    std::string resolve_covered_at(fuse_ino_t ino);

private:
    // Map DB inode to FUSE inode:
    // - root DB inode → FUSE_ROOT_ID
    // - any DB inode colliding with FUSE_ROOT_ID → generated inode
    fuse_ino_t map_db_inode(uint64_t db_ino) const
    {
        if (db_ino == root_db_ino_) return FUSE_ROOT_ID;
        if (db_ino == FUSE_ROOT_ID) return next_gen_ino_++;
        return static_cast<fuse_ino_t>(db_ino);
    }

    // Build a relative path string for a file by walking parent chain
    std::string build_relative_path(fuse_ino_t parent_ino, const std::string& name) const
    {
        std::vector<std::string> parts;
        parts.push_back(name);

        fuse_ino_t current = parent_ino;
        while (current != FUSE_ROOT_ID && current != 0) {
            auto it = inodes_.find(current);
            if (it == inodes_.end()) break;
            parts.push_back(it->second.name);
            current = it->second.parent_ino;
        }

        std::string result = "/";
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
            result += *it;
            if (it + 1 != parts.rend()) result += "/";
        }
        return result;
    }

    std::unordered_map<fuse_ino_t, FsNode>                     inodes_;
    std::unordered_map<fuse_ino_t, std::vector<std::pair<std::string, fuse_ino_t>>> children_;
    uint64_t root_db_ino_ = 0;
    mutable uint64_t next_gen_ino_ = 0;

    bool error_ = false;
    std::string error_msg_;
    std::string src_folder_;
};

// ------------------------------------------------------------------
// Helper: get CoverFs from low-level request
// ------------------------------------------------------------------

static CoverFs* get_fs(fuse_req_t req)
{
    return static_cast<CoverFs*>(fuse_req_userdata(req));
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
// resolve_covered_at — find backup file path from fuse inode
// ------------------------------------------------------------------

std::string CoverFs::resolve_covered_at(fuse_ino_t ino)
{
    auto it = inodes_.find(ino);
    if (it == inodes_.end()) return "";
    const FsNode& node = it->second;

    if (node.kind != NodeKind::File || node.backup_id <= 0 || node.db_inode == 0)
        return "";

    // 1. Get source file's full_hash from source hash.db
    std::string src_hash_path = src_db.db_folder_ + "/hash.db";
    std::optional<std::vector<uint8_t>> src_full_hash;

    {
        sqlite3* hdb = nullptr;
        if (sqlite3_open(src_hash_path.c_str(), &hdb) != SQLITE_OK) return "";
        sqlite3_exec(hdb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT full_hash FROM hashes WHERE inode = ?";
        if (sqlite3_prepare_v2(hdb, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(node.db_inode));
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
        fprintf(stderr, "resolve_covered_at: no full_hash for src_db_inode=%lu\n",
                (unsigned long)node.db_inode);
        return "";
    }

    std::string bkp_root = "";
    std::string bkp_db_folder = derive_backup_db_folder(bkp_root, src_folder_);
    fprintf(stderr, "resolve_covered_at: bkp_root='%s' bkp_db_folder='%s'\n",
            bkp_root.c_str(), bkp_db_folder.c_str());

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
                fprintf(stderr, "resolve_covered_at: found bkp_inode=%lu\n",
                        (unsigned long)bkp_inode);
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
// Helper: fill struct stat from FsNode
// ------------------------------------------------------------------

static void fill_stat(const FsNode& node, struct stat& st)
{
    memset(&st, 0, sizeof(st));
    if (node.kind == NodeKind::Dir) {
        st.st_mode  = S_IFDIR | 0755;
        st.st_nlink = 2;
    } else {
        st.st_mode  = S_IFREG | 0444;
        st.st_nlink = 1;
        st.st_size  = node.size;
        st.st_mtime = static_cast<time_t>(node.mtime);
    }
}

// ==================================================================
// Low-level FUSE callbacks
// ==================================================================

// fuse_lowlevel_ops uses: (fuse_req_t req, fuse_ino_t ino, ...)
// Error replies: fuse_reply_err(req, errno_value)

static void fs_lookup(fuse_req_t req, fuse_ino_t parent, const char* name)
{
    CoverFs* fs = get_fs(req);
    fuse_ino_t child = fs->lookup_child(parent, name);
    if (child == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    const FsNode* node = fs->lookup_ino(child);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e = {};
    e.ino = child;
    fill_stat(*node, e.attr);
    // Use reasonable timeouts for a read-only filesystem
    e.attr_timeout = 3600.0;  // 1 hour
    e.entry_timeout = 3600.0; // 1 hour

    fuse_reply_entry(req, &e);
}

static void fs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* /*fi*/)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct stat st;
    fill_stat(*node, st);
    fuse_reply_attr(req, &st, 3600.0);
}

static void fs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t offset, struct fuse_file_info* /*fi*/)
{
    CoverFs* fs = get_fs(req);
    const FsNode* dir_node = fs->lookup_ino(ino);
    if (!dir_node || dir_node->kind != NodeKind::Dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    const auto* children = fs->read_dir(ino);

    // Allocate response buffer
    std::vector<char> buf(size);
    size_t used = 0;
    off_t idx = 0;

    // "." entry at offset 0
    struct stat dot_st;
    fill_stat(*dir_node, dot_st);
    if (offset <= idx) {
        size_t remaining = size - used;
        size_t entry_size = fuse_add_direntry(req, buf.data() + used, remaining,
                                               ".", &dot_st, idx + 1);
        if (entry_size > remaining) {
            // Buffer full — caller will resume from this offset
            fuse_reply_buf(req, buf.data(), used);
            return;
        }
        used += entry_size;
    }
    ++idx;

    // ".." entry at offset 1
    if (dir_node->parent_ino != 0 || ino != FUSE_ROOT_ID) {
        const FsNode* parent_node = fs->lookup_ino(dir_node->parent_ino);
        if (parent_node) {
            struct stat parent_st;
            fill_stat(*parent_node, parent_st);
            if (offset <= idx) {
                size_t remaining = size - used;
                size_t entry_size = fuse_add_direntry(req, buf.data() + used, remaining,
                                                       "..", &parent_st, idx + 1);
                if (entry_size > remaining) {
                    fuse_reply_buf(req, buf.data(), used);
                    return;
                }
                used += entry_size;
            }
        }
    }
    ++idx;

    // Children at offsets 2+
    if (children) {
        for (const auto& [child_name, child_ino] : *children) {
            if (offset > idx) {
                ++idx;
                continue;
            }

            const FsNode* child_node = fs->lookup_ino(child_ino);
            struct stat child_st;
            if (child_node) {
                fill_stat(*child_node, child_st);
            } else {
                memset(&child_st, 0, sizeof(child_st));
                child_st.st_mode = S_IFREG | 0444;
            }

            size_t remaining = size - used;
            size_t entry_size = fuse_add_direntry(req, buf.data() + used, remaining,
                                                   child_name.c_str(), &child_st, idx + 1);
            if (entry_size > remaining) {
                // Buffer full — caller resumes from this offset
                fuse_reply_buf(req, buf.data(), used);
                return;
            }
            used += entry_size;
            ++idx;
        }
    }

    fuse_reply_buf(req, buf.data(), used);
}

static void fs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (node->kind != NodeKind::File) {
        fuse_reply_err(req, EISDIR);
        return;
    }
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        fuse_reply_err(req, EACCES);
        return;
    }
    if (node->error) {
        fuse_reply_err(req, EACCES);
        return;
    }
    fuse_reply_open(req, fi);
}

static void fs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                    off_t offset, struct fuse_file_info* /*fi*/)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (node->kind != NodeKind::File) {
        fuse_reply_err(req, EISDIR);
        return;
    }
    if (node->error || node->real_path.empty()) {
        fuse_reply_err(req, EACCES);
        return;
    }

    int fd = ::open(node->real_path.c_str(), O_RDONLY);
    if (fd < 0) {
        fuse_reply_err(req, errno);
        return;
    }

    std::vector<char> buf(size);
    ssize_t res = ::pread(fd, buf.data(), size, offset);
    if (res < 0) {
        int err = errno;
        ::close(fd);
        fuse_reply_err(req, err);
        return;
    }
    ::close(fd);

    fuse_reply_buf(req, buf.data(), static_cast<size_t>(res));
}

// ------------------------------------------------------------------
// Extended attributes
// ------------------------------------------------------------------

static void fs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    size_t needed = sizeof("user.covered");
    bool add_extra = false;
    if (node->kind == NodeKind::File
        && node->covered == static_cast<int>(covered::CoveredState::Covered)
        && node->backup_id > 0) {
        needed += sizeof("user.covered_backup") + sizeof("user.covered_at");
        add_extra = true;
    }

    if (size == 0) {
        fuse_reply_xattr(req, needed);
        return;
    }
    if (size < needed) {
        fuse_reply_err(req, ERANGE);
        return;
    }

    std::vector<char> buf(needed);
    char* p = buf.data();
    memcpy(p, "user.covered", sizeof("user.covered"));
    p += sizeof("user.covered");
    if (add_extra) {
        memcpy(p, "user.covered_backup", sizeof("user.covered_backup"));
        p += sizeof("user.covered_backup");
        memcpy(p, "user.covered_at", sizeof("user.covered_at"));
        p += sizeof("user.covered_at");
    }

    fuse_reply_buf(req, buf.data(), needed);
}

static void fs_getxattr(fuse_req_t req, fuse_ino_t ino, const char* name, size_t size)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    // user.covered
    if (strcmp(name, "user.covered") == 0) {
        const char* val = state_str(node->covered);
        size_t vlen = strlen(val);
        if (size == 0) {
            fuse_reply_xattr(req, vlen);
            return;
        }
        if (size < vlen) {
            fuse_reply_err(req, ERANGE);
            return;
        }
        fuse_reply_buf(req, val, vlen);
        return;
    }

    // user.covered_backup
    if (strcmp(name, "user.covered_backup") == 0) {
        if (node->backup_id <= 0) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        const auto& paths = fs->src_db.get_backup_paths();
        auto it = paths.find(node->backup_id);
        if (it == paths.end()) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        const std::string& val = it->second;
        size_t vlen = val.size();
        if (size == 0) {
            fuse_reply_xattr(req, vlen);
            return;
        }
        if (size < vlen) {
            fuse_reply_err(req, ERANGE);
            return;
        }
        fuse_reply_buf(req, val.c_str(), vlen);
        return;
    }

    // user.covered_at
    if (strcmp(name, "user.covered_at") == 0) {
        if (node->backup_id <= 0 || node->db_inode == 0) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        std::string full = fs->resolve_covered_at(ino);
        if (full.empty()) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        size_t vlen = full.size();
        if (size == 0) {
            fuse_reply_xattr(req, vlen);
            return;
        }
        if (size < vlen) {
            fuse_reply_err(req, ERANGE);
            return;
        }
        fuse_reply_buf(req, full.c_str(), vlen);
        return;
    }

    fuse_reply_err(req, ENODATA);
}

// ==================================================================
// main
// ==================================================================

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

    std::cerr << "Mounted " << fs.node_count() << " inodes from " << src_folder
              << " at " << mount_point << "\n";

    // Build FUSE args (options only — mount_point is passed to fuse_session_mount)
    struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, argv[0]);

    // Forward remaining options, filtering out high-level-only flags
    // (low-level API's fuse_session_loop is always foreground)
    for (int i = 3; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "-f" || opt == "-d") continue;
        fuse_opt_add_arg(&args, argv[i]);
    }

    fuse_lowlevel_ops ops = {};
    ops.lookup    = fs_lookup;
    ops.getattr   = fs_getattr;
    ops.readdir   = fs_readdir;
    ops.open      = fs_open;
    ops.read      = fs_read;
    ops.getxattr  = fs_getxattr;
    ops.listxattr = fs_listxattr;

    struct fuse_session* se = fuse_session_new(&args, &ops, sizeof(ops), &fs);
    if (!se) {
        std::cerr << "Failed to create FUSE session\n";
        fuse_opt_free_args(&args);
        return 1;
    }

    if (fuse_session_mount(se, mount_point.c_str()) != 0) {
        std::cerr << "Failed to mount FUSE filesystem\n";
        fuse_session_destroy(se);
        fuse_opt_free_args(&args);
        return 1;
    }

    fuse_session_loop(se);

    fuse_session_unmount(se);
    fuse_session_destroy(se);
    fuse_opt_free_args(&args);

    return 0;
}