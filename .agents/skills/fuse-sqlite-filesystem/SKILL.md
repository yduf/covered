---
name: fuse-sqlite-filesystem
description: additional knowledge to apply when using SQLite as metadata storage and FUSE together
---

# FUSE + SQLite Filesystem (C++) — Design Skill Guide

Instructions for the AI agent...


## Overview

This document describes how to implement a FUSE-based filesystem in C++ using SQLite strictly for metadata (inodes, directory structure, attributes), while delegating actual file data storage to an external backend.

---

## 1. High-Level Architecture

```
FUSE (kernel interface)
    ↓
C++ FUSE layer (callbacks)
    ↓
Metadata layer (SQLite)
    ↓
Content layer (files / chunks / object store)
```

### Responsibilities

* **FUSE layer**: Translates kernel syscalls into filesystem operations
* **SQLite layer**: Stores metadata only (inode, dentry, attributes)
* **Content backend**: Stores actual file data (NOT SQLite)

---

## 2. Recommended FUSE API

Use **low-level FUSE API**:

* `fuse_lowlevel_ops`

Why:

* Direct inode control
* Better mapping to SQLite inode model
* More efficient caching control

Example operations:

* lookup
* getattr
* readdir
* open
* read
* write
* create
* mkdir
* unlink
* rename

---

## 3. SQLite Schema Design

### Inodes table

```sql
CREATE TABLE inodes (
    ino         INTEGER PRIMARY KEY,
    mode        INTEGER NOT NULL,
    uid         INTEGER NOT NULL,
    gid         INTEGER NOT NULL,
    size        INTEGER NOT NULL,
    atime       INTEGER NOT NULL,
    mtime       INTEGER NOT NULL,
    ctime       INTEGER NOT NULL,
    nlink       INTEGER NOT NULL,
    data_ref    TEXT
);
```

### Directory entries table

```sql
CREATE TABLE dentries (
    parent_ino  INTEGER NOT NULL,
    name        TEXT NOT NULL,
    child_ino   INTEGER NOT NULL,
    UNIQUE(parent_ino, name)
);
```

### Key idea

* SQLite stores **metadata only**
* `data_ref` points to external storage (file path, object ID, chunk hash)

---

## 4. Metadata Layer Abstraction

Avoid direct SQLite calls inside FUSE callbacks.

```cpp
class MetadataStore {
public:
    Inode get_inode(uint64_t ino);
    Inode lookup(uint64_t parent, std::string_view name);

    void create_file(...);
    void mkdir(...);
    void unlink(...);

    std::vector<DirEntry> list_dir(uint64_t ino);
};
```

---

## 5. SQLite Performance Practices

### Always use prepared statements

* Avoid query recompilation
* Improve hot-path performance

### Enable WAL mode

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA foreign_keys=ON;
```

### Use a single shared connection

* Avoid per-callback connections
* Prevent lock contention overhead

---

## 6. Transaction Model

Filesystem operations must be atomic.

Example (rename):

```cpp
BEGIN IMMEDIATE;

// update dentries
// update inode metadata

COMMIT;
```

Rollback on failure.

---

## 7. File Data Storage Strategy

SQLite does NOT store file contents.

Possible backends:

* flat files (`/data/xx/yy/file`)
* chunk store (content-addressed)
* object storage (S3-like)

Example `data_ref`:

```
data/ab/cd/file123
s3://bucket/object
hash://<sha256>
```

---

## 8. FUSE Operation Flow

### lookup()

```
parent inode + name
    ↓
SQLite dentries
    ↓
child inode
    ↓
SQLite inode table
```

### read()

```
inode
    ↓
data_ref
    ↓
backend read
    ↓
return bytes
```

### write()

```
write to backend
    ↓
update size/mtime in SQLite
    ↓
commit transaction
```

---

## 9. Caching Strategy

### Recommended caches

* inode cache (in-memory map)
* dentry cache
* prepared statement cache

### Invalidation triggers

* write
* setattr
* rename
* unlink

---

## 10. Inode Semantics Rules

Critical correctness rules:

* inode numbers must be stable
* correct `st_nlink` tracking
* correct directory entries (`.` and `..`)
* consistent timestamps

---

## 11. Open File Handles

Store per-open state:

```cpp
struct FileHandle {
    uint64_t ino;
    BackendHandle backend;
    size_t offset;
};
```

Attach to:

```
fi->fh
```

---

## 12. Crash Consistency Model

Order of operations:

1. Write file data to backend
2. fsync backend
3. Update SQLite metadata
4. Commit transaction

Ensures consistency between metadata and data.

---

## 13. Minimal FUSE Implementation Set

Required ops for working filesystem:

* lookup
* getattr
* readdir
* open
* read
* write
* create
* mkdir
* unlink
* rename
* setattr

---

## 14. Performance Pitfalls

Avoid:

* SQL per tiny syscall without caching
* reconnecting SQLite per request
* storing file bytes in SQLite
* lack of transactions

---

## 15. Scalability Pattern

This design scales like:

```
SQLite = metadata authority
Backend = blob store
FUSE = syscall translator
```

This is similar to modern content-addressable filesystem designs.

---

## 16. Recommended Libraries

* libfuse (low-level API)
* SQLite (WAL mode)
* spdlog (logging)
* fmt (formatting)

---

## 17. Summary

This architecture cleanly separates:

* metadata (SQLite)
* data (external store)
* kernel interface (FUSE)

It is efficient, scalable, and widely used in user-space filesystem designs.
