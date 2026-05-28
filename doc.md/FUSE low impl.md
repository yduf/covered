## Root Cause Identified and Fixed

The issue was three interconnected problems in the FUSE low-level implementation (`src/fuse_main.cpp`):

### 1. Missing `release` callback
The FUSE `fuse_lowlevel_ops` table had no `release` handler. When a video player uses `mmap()`, the kernel FUSE driver keeps the file internally open for the duration of the mapping. When the player calls `munmap()`/`close()`, the kernel sends a FUSE `release` request — but with no handler registered, this request was **never answered**, causing the close syscall to hang until FUSE timeout. This made the mount appear broken to the player.

### 2. Missing `init` callback (no `FUSE_CAP_ASYNC_READ`)
The `init` callback is where the daemon negotiates capabilities with the kernel. Without it, `FUSE_CAP_ASYNC_READ` was not set, meaning the kernel serialized all read requests including mmap page-fault reads. This made mmap-based access unreliable and slow.

### 3. Inefficient open/read/lifecycle
`fs_read()` was opening the underlying file, reading, then immediately closing it on **every single read request**. For mmap, the kernel issues many small (4KB) page-fault reads — this open/close-per-page overhead degraded performance and could hit file descriptor limits.

### Changes Made (all in `src/fuse_main.cpp`):

| Callback | Before | After |
|---|---|---|
| `init` | Not implemented | Sets `FUSE_CAP_ASYNC_READ` to enable async page-fault reads |
| `open` | Only validated access, no FD stored | Opens underlying file, stores FD in `fi->fh` |
| `read` | Opened/closed FD per call | Reuses FD from `fi->fh` (persistent across calls) |
| `release` | Not implemented | Closes FD from `fi->fh`, replies success (errno 0) |

The binary compiled successfully. Remount the FUSE filesystem and the video player should now work.