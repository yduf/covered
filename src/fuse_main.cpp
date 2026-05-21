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
    std::string real_path;  // files only: absolute path on real fs for read()
    std::vector<std::string> children; // basenames of direct children (dirs+files)
};

// ------------------------------------------------------------------
// Global filesystem state
// ------------------------------------------------------------------

struct CoverFs {
    std::string root_path;
    std::unordered_map<std::string, FsNode> nodes; // vpath -> FsNode
};

static CoverFs* g_fs = nullptr;

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
// Build in-memory filesystem from DB
// ------------------------------------------------------------------

static bool build_fs(covered::Database& db, CoverFs& fs)
{
    fs.root_path = db.get_root_path().value_or("/");

    // Ensure dirs have covered column and error columns
    db.migrate_dirs_covered_column();
    db.migrate_error_columns();

    auto dirs = db.get_all_dirs();
    if (dirs.empty()) return false;

    // Compute dir covered states
    auto dir_covered = db.compute_dir_covered();

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
        node.kind    = NodeKind::Dir;
        node.covered = cov_state;
        node.error   = d.error;
        node.size    = 0;
        node.mtime   = 0;
        fs.nodes[vpath] = node;
    }

    // Insert file nodes and register as children of their dir
    auto all_files = db.get_all_files();
    for (const auto& f : all_files) {
        auto pit = dir_paths.find(f.dir_inode);
        if (pit == dir_paths.end()) continue;
        const std::string& dir_vpath = pit->second;
        std::string vpath = (dir_vpath == "/" ? "" : dir_vpath) + "/" + f.name;

        FsNode node;
        node.kind    = NodeKind::File;
        node.covered = f.covered ? static_cast<int>(covered::CoveredState::Covered)
                                 : (f.error ? static_cast<int>(covered::CoveredState::Error)
                                            : static_cast<int>(covered::CoveredState::Uncovered));
        node.error   = f.error;
        node.size    = f.size;
        node.mtime   = f.mtime;
        // Reconstruct real path from root_path
        node.real_path = fs.root_path + vpath;
        fs.nodes[vpath] = node;

        // Register as child of parent dir
        auto& dir_node = fs.nodes[dir_vpath];
        dir_node.children.push_back(f.name);
    }

    // Register child directories
    std::unordered_map<uint64_t, std::vector<uint64_t>> children_map;
    for (const auto& d : dirs) {
        if (d.parent_inode != 0)
            children_map[d.parent_inode].push_back(d.inode);
    }
    for (const auto& d : dirs) {
        if (d.parent_inode == 0) continue; // root has no parent entry
        std::string parent_vpath = dir_paths[d.parent_inode];
        auto pit = fs.nodes.find(parent_vpath);
        if (pit != fs.nodes.end()) {
            pit->second.children.push_back(d.name);
        }
    }
    return true;
}

// ------------------------------------------------------------------
// FUSE operations (forward declarations for the ops table)
// ------------------------------------------------------------------

static int cfs_getattr(const char* path, struct stat* st, struct fuse_file_info* /*fi*/)
{
    memset(st, 0, sizeof(*st));
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;

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
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;
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
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;
    if (it->second.kind != NodeKind::File) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    if (it->second.error) return -EACCES;  // cannot read files that had errors
    return 0;
}

static int cfs_read(const char* path, char* buf, size_t size, off_t offset,
                    struct fuse_file_info* /*fi*/)
{
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;
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

static int cfs_listxattr(const char* path, char* buf, size_t size)
{
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;

    const char xattr_name[] = "user.covered";
    size_t needed = sizeof(xattr_name); // includes null terminator

    if (size == 0) return static_cast<int>(needed);
    if (size < needed) return -ERANGE;
    memcpy(buf, xattr_name, needed);
    return static_cast<int>(needed);
}

static int cfs_getxattr(const char* path, const char* name, char* buf, size_t size)
{
    auto it = g_fs->nodes.find(path);
    if (it == g_fs->nodes.end()) return -ENOENT;

    if (strcmp(name, "user.covered") != 0) return -ENODATA;

    const char* val = state_str(it->second.covered);
    size_t vlen = strlen(val); // without null

    if (size == 0) return static_cast<int>(vlen);
    if (size < vlen) return -ERANGE;
    memcpy(buf, val, vlen);
    return static_cast<int>(vlen);
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

    CoverFs fs;
    if (!build_fs(db, fs)) {
        std::cerr << "Error: failed to build filesystem from database.\n";
        return 1;
    }
    g_fs = &fs;

    std::cerr << "Mounted " << fs.nodes.size() << " entries from " << src_folder
              << " at " << mount_point << "\n";

    // Build fuse argc/argv: [progname, mount_point, extra_opts...]
    std::vector<std::string> fuse_args_str;
    fuse_args_str.push_back(argv[0]);
    fuse_args_str.push_back(mount_point);
    // Forward any extra fuse options (e.g. -d, -f, -s)
    for (int i = 3; i < argc; ++i) {
        fuse_args_str.push_back(argv[i]);
    }
    // Default: run in foreground (-f) so the user can ctrl-c to unmount
    // Only add -f if not already specified
    bool has_fg = false;
    for (int i = 3; i < argc; ++i)
        if (std::string(argv[i]) == "-f" || std::string(argv[i]) == "-d") has_fg = true;
    if (!has_fg) fuse_args_str.push_back("-f");

    std::vector<char*> fuse_argv;
    for (auto& s : fuse_args_str) fuse_argv.push_back(const_cast<char*>(s.c_str()));

    // Build ops table at runtime to avoid C++20 designated initializer issues
    struct fuse_operations ops;
    memset(&ops, 0, sizeof(ops));
    ops.getattr   = cfs_getattr;
    ops.readdir   = cfs_readdir;
    ops.open      = cfs_open;
    ops.read      = cfs_read;
    ops.getxattr  = cfs_getxattr;
    ops.listxattr = cfs_listxattr;

    return fuse_main(static_cast<int>(fuse_argv.size()), fuse_argv.data(),
                     &ops, nullptr);
}