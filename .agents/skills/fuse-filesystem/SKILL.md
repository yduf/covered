---
name: fuse-filesystem
description: FUSE Filesystem Refactoring Skill Guide
---

# fuse-filesystem

Instructions for the AI agent...

## Usage

When Implementing FUSE filesystem or touching current implementation make sure to follow and apply the principle below.

If asked for refactoring, consider by order of priority action mentioned in "./doc.md/FUSE Filesystem Refactoring Tips.md"

## Overview

This document describes the architectural principles, critical methods, and engineering practices required to build and refactor a maintainable FUSE filesystem in modern C++.

The focus is not only on making the filesystem functional, but also:

* testable
* extensible
* thread-safe
* debuggable
* portable
* maintainable over time

The central architectural principle is:

> FUSE is only an adapter layer.
>
> Your filesystem logic should exist independently from FUSE.

---

# 1. Core Architecture

A clean FUSE filesystem should be layered.

Recommended structure:

```text
FUSE callbacks
    ↓
Filesystem adapter layer
    ↓
Virtual filesystem (VFS) layer
    ↓
Inode / vnode model
    ↓
Storage backend
```

## Responsibilities

### FUSE Layer

Responsible only for:

* translating POSIX calls
* converting errno values
* request marshaling
* forwarding requests

Should NOT contain:

* business logic
* path traversal logic
* caching policy
* storage management

---

### VFS Layer

Responsible for:

* dispatching operations
* inode resolution
* permissions
* file descriptor management
* object lifetime

---

### Backend Layer

Responsible for:

* persistence
* object storage
* network communication
* encryption
* compression
* block management

---

# 2. Critical FUSE Methods

The following callbacks define most filesystem semantics.

## Path Resolution

### getattr

Most important metadata operation.

Responsible for:

* file existence
* permissions
* timestamps
* file type
* ownership
* size

Poor implementations often:

* duplicate path traversal
* perform unnecessary allocations
* recompute metadata repeatedly

Refactor goal:

```cpp
Result<Inode> lookup(Path path);
```

The callback should only translate between FUSE and internal structures.

---

## readdir

One of the most error-prone callbacks.

Responsible for:

* directory iteration
* stable ordering
* offsets
* pagination
* hidden entries

Common mistakes:

* rebuilding entire directory listings repeatedly
* ignoring offsets
* mixing iteration with storage access

Preferred abstraction:

```cpp
class DirectoryIterator {
public:
    bool next(DirEntry& entry);
};
```

---

## open

Responsible for:

* validating access
* allocating file handle state
* initializing caches
* tracking open descriptors

Should NOT:

* read entire files eagerly
* perform unrelated metadata updates

Recommended pattern:

```cpp
class FileHandle {
public:
    virtual ~FileHandle() = default;
};
```

Store handles inside:

```cpp
fi->fh
```

using RAII.

---

## read

Critical data path.

Responsibilities:

* reading byte ranges
* offset management
* cache interaction
* sparse file handling

Should avoid:

* unnecessary copies
* path traversal
* repeated inode lookup

Preferred API:

```cpp
Result<size_t> read(InodeId inode,
                    std::span<std::byte> buffer,
                    uint64_t offset);
```

---

## write

Must define clear consistency semantics.

Questions to answer:

* write-through or write-back?
* synchronous or buffered?
* partial write behavior?
* crash consistency?

Should separate:

* page cache logic
* backend persistence
* metadata updates

---

## release

Responsible for cleanup.

Important distinction:

* `flush` synchronizes state
* `release` destroys state

Generated code often confuses these.

---

## rename

One of the hardest operations.

Must correctly handle:

* atomicity
* directory movement
* overwrite semantics
* hard links
* cache invalidation

Should be implemented transactionally whenever possible.

---

# 3. Path Handling

Raw `const char* path` usage becomes unmaintainable quickly.

Introduce a dedicated path abstraction.

Example:

```cpp
class Path {
public:
    bool is_root() const;
    Path parent() const;
    std::vector<std::string_view> components() const;
};
```

Centralize:

* normalization
* UTF-8 validation
* slash collapsing
* `.` and `..` resolution

Avoid duplicated parsing logic.

---

# 4. Inode Model

Avoid path-centric architectures.

Real filesystems operate primarily on inode-like objects.

Example:

```cpp
struct Inode {
    InodeId id;
    FileType type;
    Permissions perms;
    uid_t uid;
    gid_t gid;
    size_t size;
};
```

Benefits:

* easier caching
* stable identity
* efficient hard links
* simpler rename semantics
* lower lookup cost

---

# 5. Error Handling

Avoid propagating POSIX errno values through the entire codebase.

Bad:

```cpp
return -ENOENT;
```

Preferred:

```cpp
enum class FsError {
    NotFound,
    PermissionDenied,
    AlreadyExists
};
```

Translate errors only at the FUSE boundary.

Benefits:

* cleaner APIs
* easier testing
* backend portability
* improved readability

---

# 6. Concurrency

FUSE is multithreaded by default.

Many generated skeletons are not thread-safe.

Audit:

* inode tables
* global caches
* open handle maps
* lazy initialization
* logging

Recommended tools:

* `std::mutex`
* `std::shared_mutex`
* immutable structures
* lock hierarchy documentation

Avoid:

* hidden shared state
* global mutable objects
* unsynchronized caches

---

# 7. Memory Management

Generated C-style code often leaks ownership semantics.

Prefer:

* `std::unique_ptr`
* `std::vector`
* `std::string`
* `std::optional`
* `std::span`
* RAII wrappers

Avoid:

* raw `malloc/free`
* naked `new/delete`
* implicit ownership transfer

---

# 8. Logging and Observability

Filesystem debugging is difficult without tracing.

Add structured logging early.

Example:

```cpp
LOG_DEBUG("read inode={} offset={} size={}",
          inode.id,
          offset,
          size);
```

Useful diagnostics:

* operation latency
* inode resolution traces
* cache hit/miss ratio
* backend IO timing
* lock contention

---

# 9. Testing Strategy

Your filesystem should be testable without mounting it.

## Unit Tests

Test:

* path normalization
* inode lookup
* permission checks
* rename semantics
* cache logic

without FUSE.

---

## Integration Tests

Mount filesystem in temporary directory.

Test:

* POSIX compliance
* concurrent access
* symlink behavior
* crash recovery
* large directory traversal

---

# 10. Common Refactoring Targets

## Remove giant callbacks

Callbacks should only:

* validate inputs
* invoke services
* translate errors

---

## Centralize lookup

Avoid repeated path traversal.

Implement:

```cpp
lookup(path)
```

once.

---

## Eliminate global state

Prefer dependency injection.

Bad:

```cpp
static Filesystem* g_fs;
```

Preferred:

```cpp
FuseContext {
    Filesystem& fs;
};
```

---

## Remove duplicated code

Generated code often duplicates:

* permission checks
* path parsing
* inode resolution
* logging
* metadata conversion

Centralize policies.

---

# 11. Recommended Internal Interfaces

## Filesystem Service

```cpp
class Filesystem {
public:
    Result<Inode> lookup(Path path);

    Result<size_t> read(InodeId inode,
                        std::span<std::byte> buffer,
                        uint64_t offset);

    Result<size_t> write(InodeId inode,
                         std::span<const std::byte> buffer,
                         uint64_t offset);
};
```

---

## Backend Interface

```cpp
class StorageBackend {
public:
    virtual Result<Block> read_block(BlockId id) = 0;
    virtual Result<void> write_block(BlockId id,
                                     const Block& block) = 0;
};
```

---

# 12. Long-Term Scalability

Design early for:

* large directories
* millions of inodes
* remote backends
* partial loading
* asynchronous IO
* snapshots
* journaling
* metadata caching

Even if not implemented immediately.

---

# 13. Recommended Refactoring Order

## Phase 1 — Structural Cleanup

* isolate FUSE adapter
* centralize path lookup
* introduce inode model
* remove global state

---

## Phase 2 — Correctness

* fix lifecycle semantics
* improve error handling
* audit thread safety
* add tests

---

## Phase 3 — Performance

* metadata caching
* directory iteration optimization
* read/write buffering
* lock contention reduction

---

## Phase 4 — Advanced Features

* async backend
* journaling
* snapshot support
* distributed storage

---

# 14. Final Engineering Principle

A maintainable FUSE filesystem should satisfy the following condition:

> The filesystem core should continue functioning even if FUSE is replaced.

If your implementation can:

* be unit-tested independently
* run without mounting
* swap storage backends cleanly
* support multiple frontends

then the architecture is usually healthy.
