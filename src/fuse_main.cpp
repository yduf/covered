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
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include "db.hpp"
#include "commands.hpp"

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
    covered::HashDatabase src_hash_; // source hash.db
    std::unordered_map<int, covered::HashDatabase> bck_db; // backup_id -> backup

    CoverFs(const std::string& src_folder) :
        src_db(std::filesystem::absolute(src_folder).string() + "/filesize.db"),
        src_hash_(std::filesystem::absolute(src_folder).string() + "/hash.db")
    {
        src_folder_ = std::filesystem::absolute(src_folder).string();
        // Strip trailing slash — parent_path() misbehaves on "/dir/"
        while (!src_folder_.empty() && src_folder_.back() == '/') {
            src_folder_.pop_back();
        }

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

            // Build real path for directories too (needed for user.covered_source xattr)
            std::string dir_real_path;
            if (parent_ino == 0) {
                dir_real_path = root_path;
            } else {
                dir_real_path = root_path + build_relative_path(parent_ino, d.name);
            }

            FsNode node;
            node.kind      = NodeKind::Dir;
            node.covered   = cov_state;
            node.error     = d.error;
            node.size      = 0;
            node.mtime     = 0;
            node.db_inode  = d.inode;
            node.parent_ino = parent_ino;
            node.backup_id = 0;
            node.real_path = dir_real_path;
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
                // Don't include the root directory name (already in root_path from config.json)
                uint64_t parent = sqlite3_column_type(stmt, 0) == SQLITE_NULL
                                      ? 0
                                      : static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                if (parent != 0 && name) parts.push_back(name);
                sqlite3_finalize(stmt);
                stmt = nullptr;

                while (parent != 0) {
                    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(parent));
                        if (sqlite3_step(stmt) == SQLITE_ROW) {
                            const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                            parent = sqlite3_column_type(stmt, 0) == SQLITE_NULL
                                         ? 0
                                         : static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                            // Don't include the root directory name (already in root_path)
                            if (parent != 0 && n) parts.push_back(n);
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

    if (parts.empty()) return root_path;

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

    // 1. Get source file's full_hash from source hash.db via src_hash_
    auto src_full_hash = src_hash_.get_full_hash(node.db_inode);
    if (!src_full_hash.has_value() || src_full_hash->empty())
        return "";

    // 2. Find the backup hash.db for this file's backup_id
    auto bkp_it = bck_db.find(node.backup_id);
    if (bkp_it == bck_db.end() || bkp_it->second.has_error())
        return "";
    covered::HashDatabase& bkp_hash = bkp_it->second;

    // 3. Find matching inode in backup hash.db via indexed full_hash
    auto bkp_inode_opt = bkp_hash.find_inode_by_full_hash(*src_full_hash);
    if (!bkp_inode_opt.has_value() || *bkp_inode_opt == 0)
        return "";
    uint64_t bkp_inode = *bkp_inode_opt;

    // 4. Look up the backup file's dir_inode and name from backup filesize.db
    std::string bkp_root = src_db.get_backup_path(node.backup_id);
    std::string bkp_db_folder = derive_backup_db_folder(bkp_root, src_folder_);

    std::string bkp_file_name;
    uint64_t bkp_dir_inode = 0;
    {
        std::string bkp_filesize_path = bkp_db_folder + "/filesize.db";
        sqlite3* fdb = nullptr;
        if (sqlite3_open(bkp_filesize_path.c_str(), &fdb) != SQLITE_OK) return "";
        sqlite3_exec(fdb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT dir_inode, name FROM files WHERE inode = ?";
        if (sqlite3_prepare_v2(fdb, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(bkp_inode));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                bkp_dir_inode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
                const char* fname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (fname) bkp_file_name = fname;
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(fdb);
    }

    if (bkp_dir_inode == 0 || bkp_file_name.empty())
        return "";

    // 5. Build full path from backup filesize.db using dir_inode
    std::string dir_path = build_backup_path(bkp_dir_inode, bkp_db_folder);
    if (dir_path.empty())
        return "";

    std::string result = dir_path;
    if (!result.empty() && result.back() != '/') result += '/';
    result += bkp_file_name;
    return result;
}

// ------------------------------------------------------------------
// Helper: fill struct stat from FsNode
// ------------------------------------------------------------------

static void fill_stat(const FsNode& node, struct stat& st,
                      const struct fuse_ctx* ctx = nullptr)
{
    memset(&st, 0, sizeof(st));
    if (node.kind == NodeKind::Dir) {
        st.st_mode  = S_IFDIR | 0755;
        st.st_nlink = 2;
    } else {
        st.st_mode  = S_IFREG | 0644;
        st.st_nlink = 1;
        st.st_size  = node.size;
        st.st_mtime = static_cast<time_t>(node.mtime);
        st.st_blksize = 4096;
        st.st_blocks  = (node.size + 511) / 512;
    }

    // Set ownership from caller context so files are not root-owned.
    // ffprobe/FFmpeg rejects root-owned files with EACCES.
    if (ctx != nullptr) {
        st.st_uid = ctx->uid;
        st.st_gid = ctx->gid;
    }
}

// ------------------------------------------------------------------
// Per-open-file-handle state — tracks underlying fd + kernel position
// ------------------------------------------------------------------

struct FileHandle {
    int fd = -1;
    off_t pos = 0;   // kernel-visible file position (for SEEK_CUR / lseek offset 0)
};

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
    fill_stat(*node, e.attr, fuse_req_ctx(req));
    // Use reasonable timeouts for a read-only filesystem
    e.attr_timeout = 3600.0;  // 1 hour
    e.entry_timeout = 3600.0; // 1 hour

    fuse_reply_entry(req, &e);
}

static void fs_access(fuse_req_t req, fuse_ino_t ino, int mask)
{
    // Always grant access. The underlying real file determines actual
    // readability. Kernel-side permission checks on uid/gid/mode in
    // the FUSE stat can reject files even when the daemon owns them —
    // ffprobe/FFmpeg explicitly uses access() which hits this path.
    (void)ino;
    (void)mask;
    fuse_reply_err(req, 0);
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
    fill_stat(*node, st, fuse_req_ctx(req));
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
    fill_stat(*dir_node, dot_st, fuse_req_ctx(req));
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
            fill_stat(*parent_node, parent_st, fuse_req_ctx(req));
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
        const struct fuse_ctx* ctx = fuse_req_ctx(req);
        for (const auto& [child_name, child_ino] : *children) {
            if (offset > idx) {
                ++idx;
                continue;
            }

            const FsNode* child_node = fs->lookup_ino(child_ino);
            struct stat child_st;
            if (child_node) {
                fill_stat(*child_node, child_st, ctx);
            } else {
                memset(&child_st, 0, sizeof(child_st));
                child_st.st_mode = S_IFREG | 0644;
                child_st.st_uid = ctx->uid;
                child_st.st_gid = ctx->gid;
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

static void fs_init(void* userdata, struct fuse_conn_info* conn)
{
    // Enable async reads for mmap page-fault handling.
    // Enable splice_read for zero-copy data transfer from page cache —
    // reduces latency for media probing (many small reads).
    conn->want |= FUSE_CAP_ASYNC_READ | FUSE_CAP_SPLICE_READ;
    (void)userdata;
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
    // Open the underlying file once and allocate a FileHandle.
    // The FileHandle tracks the kernel-visible file position (for lseek).
    // Stored in fi->fh, reused across reads, closed in fs_release.
    int fd = ::open(node->real_path.c_str(), O_RDONLY);
    if (fd < 0) {
        fuse_reply_err(req, errno);
        return;
    }
    auto* fh = new FileHandle;
    fh->fd = fd;
    fh->pos = 0;
    fi->fh = reinterpret_cast<uint64_t>(fh);
    fi->direct_io = 0;
    fi->keep_cache = 1;     // retain kernel page cache across open/close cycles
    fuse_reply_open(req, fi);
}

static void fs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                    off_t offset, struct fuse_file_info* fi)
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
    if (node->real_path.empty()) {
        fuse_reply_err(req, EACCES);
        return;
    }

    auto* fh = reinterpret_cast<FileHandle*>(fi->fh);
    if (!fh || fh->fd < 0) {
        fuse_reply_err(req, EBADF);
        return;
    }

    // Use pread for atomic, stateless offset-based reads.
    // pread does NOT modify the kernel file offset, so read requests
    // from concurrent threads (common in media frameworks) cannot race.
    std::vector<char> buf(size);
    ssize_t res = ::pread(fh->fd, buf.data(), size, offset);
    if (res < 0) {
        fuse_reply_err(req, errno);
        return;
    }

    // Track the logical position for lseek(SEEK_CUR) replies.
    fh->pos = offset + static_cast<off_t>(res);

    fuse_reply_buf(req, buf.data(), static_cast<size_t>(res));
}

static void fs_release(fuse_req_t req, fuse_ino_t /*ino*/, struct fuse_file_info* fi)
{
    auto* fh = reinterpret_cast<FileHandle*>(fi->fh);
    if (fh) {
        if (fh->fd >= 0) {
            ::close(fh->fd);
        }
        delete fh;
    }
    fi->fh = 0;
    fuse_reply_err(req, 0);
}

static void fs_lseek(fuse_req_t req, fuse_ino_t ino, off_t offset, int whence,
                     fuse_file_info* fi)
{
    CoverFs* fs = get_fs(req);
    const FsNode* node = fs->lookup_ino(ino);
    if (!node || node->kind != NodeKind::File) {
        fuse_reply_err(req, EBADF);
        return;
    }

    auto* fh = reinterpret_cast<FileHandle*>(fi->fh);
    if (!fh || fh->fd < 0) {
        fuse_reply_err(req, EBADF);
        return;
    }

    off_t new_pos = 0;
    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = fh->pos + offset;
        break;
    case SEEK_END:
        new_pos = node->size + offset;
        break;
    default:
        fuse_reply_err(req, EINVAL);
        return;
    }

    if (new_pos < 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    fh->pos = new_pos;
    fuse_reply_lseek(req, new_pos);
}

static void fs_flush(fuse_req_t req, fuse_ino_t /*ino*/, struct fuse_file_info* /*fi*/)
{
    // Read-only filesystem — flush is a no-op.
    // Must reply 0 so close() succeeds (some apps, including mpv/celluloid,
    // treat a failing close as a fatal error even for read-only files).
    fuse_reply_err(req, 0);
}

static void fs_statfs(fuse_req_t req, fuse_ino_t /*ino*/)
{
    // Provide minimal filesystem stats. Celluloid/mpv probes statfs and
    // may refuse to open files if the call returns ENOSYS.
    struct statvfs st = {};
    st.f_bsize  = 4096;
    st.f_frsize = 4096;
    st.f_namemax = 255;
    // f_blocks, f_bfree, f_bavail, f_files, f_ffree left at 0.
    // "zero free blocks" is fine for a read-only, virtual fs.
    fuse_reply_statfs(req, &st);
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
    bool add_source = false;
    if (!node->real_path.empty()) {
        needed += sizeof("user.covered_source");
        add_source = true;
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
    if (add_source) {
        memcpy(p, "user.covered_source", sizeof("user.covered_source"));
        p += sizeof("user.covered_source");
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

    // user.covered_source — underlying source filesystem path
    if (strcmp(name, "user.covered_source") == 0) {
        if (node->real_path.empty()) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        size_t vlen = node->real_path.size();
        if (size == 0) {
            fuse_reply_xattr(req, vlen);
            return;
        }
        if (size < vlen) {
            fuse_reply_err(req, ERANGE);
            return;
        }
        fuse_reply_buf(req, node->real_path.c_str(), vlen);
        return;
    }

    fuse_reply_err(req, ENODATA);
}

// ==================================================================
// main
// ==================================================================

int cmd_fuse(int argc, char* argv[])
{
    // We expect: cover fuse <source_folder> <mount_point> [fuse options]
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
    ops.init      = fs_init;
    ops.lookup    = fs_lookup;
    ops.getattr   = fs_getattr;
    ops.access    = fs_access;
    ops.readdir   = fs_readdir;
    ops.open      = fs_open;
    ops.read      = fs_read;
    ops.release   = fs_release;
    ops.flush     = fs_flush;
    ops.lseek     = fs_lseek;
    ops.statfs    = fs_statfs;
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