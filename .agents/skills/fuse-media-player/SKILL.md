---
name: fuse-media-player
description: Debugging FUSE Filesystems to support Multimedia Applications
---
# skill.md — Debugging FUSE Filesystems for Multimedia Applications

## Problem

A FUSE filesystem appears functional:

* files are visible
* metadata looks correct
* file contents can be read

…but multimedia applications such as Celluloid, mpv, VLC, ffplay, or GStreamer-based tools fail to open or play media files.

This usually means the filesystem implementation violates assumptions expected by media frameworks rather than exposing invalid data.

---

# Core Multimedia Filesystem Expectations

Modern media frameworks aggressively probe filesystems.

Typical operations include:

* random seeks
* repeated opens
* tiny reads at arbitrary offsets
* metadata probing
* reads near EOF
* inode caching
* mmap-backed access patterns

A filesystem that works for `cat` may still fail for FFmpeg/GStreamer.

---

# Common Failure Points

## 1. Broken Offset-Based Reads

### Symptom

Media probing fails or playback never starts.

### Cause

`read()` behaves sequentially instead of respecting the requested offset.

Incorrect implementation example:

```c
read(fd, buf, size, current_position)
```

where internal mutable state determines the next bytes returned.

Correct behavior:

`read()` must behave like `pread()`:

* every request is independent
* offset fully determines data location
* repeated reads at same offset must return identical data

### Requirements

* arbitrary offsets must work
* backwards seeks must work
* overlapping reads must work
* sparse/random access patterns must work

---

## 2. Incorrect File Size (`st_size`)

### Symptom

Players reject file immediately.

### Cause

`getattr()` reports:

* size = 0
* unstable size
* estimated size
* dynamically changing size

### Requirements

```c
stbuf->st_size
```

must be:

* exact
* stable
* available before playback begins

Especially important for:

* MP4
* MKV
* fragmented containers

---

## 3. Incorrect EOF Handling

### Symptom

Infinite probing loops or corrupted playback.

### Correct Behavior

EOF occurs only when:

```c
offset >= filesize
```

and then:

```c
return 0;
```

### Common Bugs

* returning partial garbage
* returning success past EOF
* returning errors instead of EOF

---

## 4. Seek Support

### Symptom

Metadata probing fails.

### Cause

Media frameworks seek extensively.

Typical access patterns:

* read beginning
* seek near end
* jump back
* inspect metadata atoms

MP4 specifically often seeks to:

* `moov`
* `mdat`
* trailer metadata

### Requirements

Filesystem must support:

* arbitrary seeks
* reverse seeks
* large-offset seeks

---

## 5. `open()` Flag Handling

### Symptom

Open succeeds in shell tools but fails in media players.

### Cause

Applications may open files using:

```c
O_RDONLY
O_NONBLOCK
```

or reopen files multiple times.

### Recommendations

Avoid rejecting unknown or extra flags unnecessarily.

Typical safe defaults:

```c
fi->direct_io = 0;
fi->keep_cache = 1;
```

unless direct I/O is explicitly required.

---

## 6. Direct I/O Problems

### Symptom

Playback instability or probing failures.

### Cause

Many media stacks assume page-cache behavior.

Using:

```c
direct_io
```

can expose subtle alignment and buffering issues.

### Recommendation

Disable direct I/O initially.

Prefer kernel caching unless there is a strong reason otherwise.

---

## 7. Inode Instability

### Symptom

Strange caching or repeated probing behavior.

### Cause

`st_ino` changes between calls.

Some frameworks cache media objects using inode identity.

### Requirement

Provide stable inode numbers.

Bad:

```c
st_ino = random();
```

Good:

```c
st_ino = deterministic_hash(path);
```

or persistent inode mapping.

---

## 8. `readdir()` Metadata Inconsistency

### Symptom

File appears but cannot be opened correctly.

### Cause

Directory entry type differs from actual file type.

### Recommendations

Use:

```c
FUSE_FILL_DIR_PLUS
```

when available.

Ensure consistency between:

* `readdir`
* `lookup`
* `getattr`

---

## 9. Missing `flush()` / `release()`

### Symptom

Descriptor lifecycle issues.

### Cause

Applications expect proper close semantics.

### Recommendation

Implement:

* `flush`
* `release`

even if they are mostly no-ops.

---

## 10. mmap Compatibility

### Symptom

Playback fails only in certain frameworks.

### Cause

Some media stacks internally rely on mmap-style access.

### Requirements

Filesystem reads must tolerate:

* page-aligned reads
* arbitrary offsets
* repeated requests
* parallel access

---

## 11. Permission and Mode Bits

### Symptom

Desktop environments or media frameworks ignore files.

### Correct Values

Regular file:

```c
S_IFREG | 0644
```

Directory:

```c
S_IFDIR | 0755
```

### Common Bugs

* missing type bits
* executable-only files
* invalid mode combinations

---

## 12. Extended Attributes (Less Common)

Some desktop stacks inspect xattrs such as:

* MIME type
* thumbnail metadata
* GVFS metadata

Usually optional, but useful for integration.

---

# Recommended Mount Options

## Kernel Cache

Try:

```bash
-o kernel_cache
```

or:

```bash
-o auto_cache
```

depending on workload.

Kernel caching often improves compatibility with media software.

---

# Essential Debugging Workflow

## 1. Test with ffprobe

```bash
ffprobe video.mp4
```

This often reveals probing failures immediately.

---

## 2. Trace System Calls

```bash
strace -f celluloid
```

Look for:

* `EINVAL`
* `ENOSYS`
* failed seeks
* repeated opens
* short reads

---

## 3. Compare Multiple Players

Test:

```bash
mpv video.mp4
vlc video.mp4
ffplay video.mp4
```

Different players stress filesystems differently.

---

# High-Probability Root Causes

In real-world FUSE media bugs, the most common causes are:

1. Broken offset handling in `read()`
2. Incorrect `st_size`
3. Seek failures
4. Unstable inode numbers
5. `direct_io` issues
6. Incorrect EOF semantics
7. Rejecting reopen patterns

---

# Minimal Correctness Checklist

## `getattr()`

* stable inode
* exact file size
* correct mode bits

## `read()`

* offset-based
* stateless
* arbitrary seeks supported
* proper EOF handling

## `open()`

* tolerate common flags
* allow multiple opens

## Mounting

* try kernel cache enabled
* disable direct I/O first

---

# Practical Diagnostic Hint

If:

```bash
cat video.mp4 > /dev/null
```

works but:

```bash
ffprobe video.mp4
```

fails,

the issue is almost certainly:

* seek handling
* offset correctness
* metadata probing compatibility

rather than raw data corruption.

