# Covered

A Tool to check if I have a backup of my file somewhere.

Compare content of source & backup
and list file in source that do not exist anywhere on backup.

For every file covered can also:
- highlight place where it exist on backup
- highlight duplicates on sources

# Principle

For source and backup, 
build a database of files

first run to scan them 
- scan source `./build/covered_scan_size /media/yves/Big`
- scan backup `./build/covered_scan_size /nfs/tronaut/mnt_Backup`

then run to match them ` ./build/covered_match covered_media_yves_Big/ covered_nfs_tronaut_mnt_Backup/`

## DB

There is 2 kind of usage

Source DB may be assumed to live just for the report and much smaller than backup DB.

Backup DB could be long lived, since we may check several source to it.
- it may be sparse in terms of full hash, since we only want to check source files (not necessary all DB content)

hash in DB is blake3 function, stored in a BLOB.

## Filtering Phase

## File Size retrieval

We want to build a full picture of source & backup content,
that at least give for each file its size:
Because in second step we will cluster all files by their size, so we only have to mach each source cluster to backup cluster of the exact same size.

## Match

In second step,
we retrieve file cluster on source db and backup db, and process them, trying to match all files belonging to cluster of exact same size.

We still need too quickly reject pair that do not match.
- to that compare only a blake3 hash of the first 1024 bytes of each file.

To do it:

create a db containing (size, inode, blake3( first 1024 byte)) for the filesystem

For each source cluster size
  fill the above DB for source filesystem
  do the same on the backup DB (but only for size matching source cluster)

Then 
For each source cluster size,
  retrieve sorted hash for first byte on source DB
  retrieve sorted hash for first byte on backup DB
  if they match
    proceed to compute both source and backup file using blake3 hash
    save full hash in both source and backup DB for these files

Then
for each full hash computed in source
  look to see if it exist in backup
    if match position covered flag in source DB as T, otherwise it stay null

## Report

For file in source DB
report file for which covered flag is null

# Isolation

- preparing the initial db that contains file size is one process
- clustering by size is an other process
  - filling hash for set of file matching size is an other process
- report is an other process, that can be launched independantly (it just need source and backup DB)

# Details

## DB

For each scanned filesystem folder, a dedicated output directory is created
(e.g. `covered_home_yves/`). Inside that directory you will find:

| File | Purpose |
|---|---|
| `config.json` | Small JSON file with the scanned root path: `{"root":"/absolute/path"}`. Useful for external scripts and for reconstructing filesystem paths during matching. |
| `filesize.db` | SQLite database containing the directory tree, file metadata, and the final `covered` flag. |
| `hash.db` | SQLite database caching blake3 hashes per inode (head hash and full hash). Kept separate so the match phase can be re-run or resumed incrementally. |

### `config.json`
```json
{"root":"/home/yves"}
```

### `filesize.db` tables

#### `meta`
| Column | Type | Description |
|---|---|---|
| `key` | TEXT PRIMARY KEY | Metadata key |
| `value` | INTEGER | Metadata value |

Rows:
- `device` → the `st_dev` of the scanned filesystem (prevents crossing mount points)
- `root_path` → absolute path of the scanned root directory

#### `dirs`
| Column | Type | Description |
|---|---|---|
| `inode` | INTEGER PRIMARY KEY | Directory inode |
| `parent_inode` | INTEGER | Parent directory inode (`NULL` for root) |
| `name` | TEXT | Directory basename |

#### `files`
| Column | Type | Description |
|---|---|---|
| `dir_inode` | INTEGER | Containing directory inode (part of PK) |
| `name` | TEXT | File basename (part of PK) |
| `inode` | INTEGER | File inode |
| `size` | INTEGER | File size in bytes |
| `mtime` | INTEGER | Modification time (seconds since epoch) |
| `covered` | INTEGER DEFAULT 0 | `1` if a matching file exists in backup, `0` otherwise |

Indexes: `idx_files_inode(inode)`, `idx_files_size(size)`

### `hash.db` tables

#### `hashes`
| Column | Type | Description |
|---|---|---|
| `inode` | INTEGER PRIMARY KEY | File inode |
| `head_hash` | BLOB | blake3 hash of the first 2048 bytes (`NULL` until computed) |
| `full_hash` | BLOB | blake3 hash of the entire file (`NULL` until computed) |

Both hash columns are populated lazily during the match phase and cached for reuse.
