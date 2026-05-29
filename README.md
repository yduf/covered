# Covered ☂️

A Tool to check if I have a backup of my files somewhere.

Comes with an extension for [Nemo](https://github.com/linuxmint/nemo#nemo) (the linuxmint file-manager) so it's easy to have a visual cue of what is _covered_ or not, and it can be used to directly access (for copying) the missing parts.

![nemo](/doc/nemo-extension.png)

## Usage

You may have a proper backup system in place (using Pika or Borg backup for eg), and you also may have
video, photo and documents scattered accross various media, that you use as a backup (especially if you keep your
old disk on a shelves).   
This tools is here to help solve the second situation, where you want to know if a file you have on an (external) device, is present on one of you trusty backup or not and have that information before taking the decision to delete it to get some space.

For source and backup, 
these tools will build a database of existing files and compare them.
It needs to have the source filesystem  and the backup file system reachable.

**First scan them:**
- scan source `./build/cover scan /media/yves/Big`
- scan backup `./build/cover scan /nfs/tronaut/mnt_Backup`

**Match them:**    
`./build/cover match covered_media_yves_Big/ covered_nfs_tronaut_mnt_Backup/`

**Report:**     
`./build/cover report covered_media_yves_Big/`
or list of uncovered files `./build/cover report -r covered_media_yves_Big/`

Easiest way is now to look at the result with Nemo. 

Firt install the nemo extension (this only need to be done once and `meson install` will take of it, see [Installation](#installation))

```sh
sudo cp nemo-extension/nemo-covered.py /usr/share/nemo-python/extensions/
nemo -q && nemo
```

Mount the report with FUSE:
- `mkdir -p /tmp/covered_mount`
- `./build/cover fuse covered_media_yves_Big/ /tmp/covered_mount`


And now in Nemo go to `/tmp/covered_mount`
and you should see something like the screen shot above.

You can also Check xattr user.covered from command line: 
`getfattr -n user.covered /tmp/covered_mount/some/file.txt`

**Unmount**  
`Ctrl-C` or `fusermount3 -u /tmp/covered_mount` when done

You can match one source to multiple different backup, the first match will be memorized. The backup DB are memorized as well in the source DB, so the nemo extension can help you find where is your backup.

### Remote Drive

If you are able to run the scanner on the remote host,
i supports an optional `--compute-hash` flag that computes both
`head_hash` and `full_hash` (blake3) for each file immediately during scanning,
storing them in `hash.db`. 

This is especially useful for remote filesystems
(e.g. NFS, CIFS) where you want to minimize the number of passes over the data.
Without this flag, hashes are lazily computed later during the match phase.

- scan with hashes `./build/cover scan --compute-hash /media/yves/Big`

# Install

You need to compile it.
Yet it should be fast and as simple as:

```bash
$ git git@github.com:yduf/covered.git
$ cd covered
$ meson setup build
$ cd build && meson compile
$ sudo meson install
```

# Alternatives

- [Krokiet / Czkawka](https://github.com/qarmin/czkawka#features)
- [rdfind](https://rdfind.pauldreik.se/) 
- [fdupes](https://github.com/adrianlopezroche/fdupes)
- [Duff](http://duff.dreda.org/)


These are tools that are made to find and resolve duplicates,
I didn't find them suitable for my particular use case.

In general they use the same principle to do that resolution.


# Principle

## DB

There is 2 kind of usage

Source DB may be assumed to live just for the report and much smaller than backup DB.

Backup DB could be long lived, since we may check several source to it.
- it may be sparse in terms of full hash, since we only want to check source files (not necessary all DB content)

hash in DB is blake3 function, stored in a BLOB.

## Filtering Phase

### File Size retrieval

We want to build a full picture of source & backup content,
that at least give for each file its size:
Because in second step we will cluster all files by their size, so we only have to mach each source cluster to backup cluster of the exact same size.

### Match

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

The report is done with 2 components.
- A FUSE filesystem that show the arboresence contained in src db, and expose for directory & file a custome attribute xattr: user.covered, which can take 5 states:
  For files
    - covered (file has been found in backup)
    - uncovered (file has not been found)
    - error (file could not be accessed — permission denied, IO error, etc.)
  For Folder
    - covered (all files in folder are covered)
    - uncovered (no file in this folder are covered)
    - partial (some files in folder are covered but not all)
    - empty (folder contains no files at all, recursively)
    - error (folder could not be scanned — permission denied, etc.; treated as uncovered for parent coverage)

- A Nemo extension that take into account the xattr from the fuse filesystem and:
  - For **files**: adds a colour emblem
    - green emblem if covered
    - red emblem if uncovered
    - crossed-circle emblem if error (file could not be read)
  - For **folders**: sets a coloured folder icon (via nemo-folder-color-switcher mechanism) AND an emblem
    - green folder + green emblem if all files are covered
    - red folder + red emblem if no files are covered
    - orange folder + orange emblem if partially covered
    - cyan folder (no emblem) if empty (no files at all)
    - grey folder + crossed-circle emblem if error (folder could not be scanned)

## Isolation

- preparing the initial db that contains file size is one process
- clustering by size is an other process
  - filling hash for set of file matching size is an other process
- report is an other process, that can be launched independantly (it just need source and backup DB)

# CLI

The `cover` command is the top-hat entry point. All subcommands are accessed through it:

```
cover <subcommand> [args...]
```

| Subcommand | Legacy executable | Description |
|---|---|---|
| `scan`    | `covered_scan_size` | Scan a filesystem directory |
| `match`   | `covered_match`     | Match a source DB against a backup DB |
| `report`  | `cover_report`      | Compute and display coverage statistics |
| `fuse`    | `cover_fuse`        | Mount the covered filesystem via FUSE |

The legacy executables remain available for backward compatibility.

## `cover scan`

```
cover scan [-f|--force] [--compute-hash] <folder>
```

| Option | Description |
|---|---|
| `-f` / `--force` | Overwrite existing database |
| `--compute-hash` | Compute head and full blake3 hashes during scan |

## `cover match`

```
cover match [-d] <source_folder> <backup_folder>
```

## `cover report`

Computes the covered status for every **directory** in the source DB by
aggregating its files and sub-directories (bottom-up), then writes the result
back to the `dirs.covered` column.

```
cover report [--report|-r] <source_folder>
```

| Option | Description |
|---|---|
| `--report` / `-r` | Also print the full path of every uncovered file to stdout |

Output example:
```
Source: /media/yves/Big/
Files   : 13146  covered=267  uncovered=12879  error=5
Dirs    : 1870   covered=118  partial=60  uncovered=1692  empty=42  error=3
```

## `cover fuse`

Mounts the source arborescence as a read-only FUSE filesystem.
Every file and directory exposes a `user.covered` extended attribute:

| Node | Value | Meaning |
|---|---|---|
| file | `covered` | file was found in backup |
| file | `uncovered` | file was not found in backup |
| file | `error` | file could not be accessed (permission etc.) |
| dir  | `covered` | all files under this dir are covered |
| dir  | `partial` | some files are covered, some are not |
| dir  | `uncovered` | no file under this dir is covered |
| dir  | `empty` | dir (and all sub-dirs) contain no files at all |
| dir  | `error` | dir could not be scanned (permission etc.) |

```
cover fuse <source_folder> <mount_point> [fuse options]
```

```sh
mkdir -p /tmp/covered_mount
./build/cover fuse covered_media_yves_Big/ /tmp/covered_mount
# runs in foreground; Ctrl-C to stop, or:
fusermount3 -u /tmp/covered_mount
```

## Nemo extension (`nemo-extension/nemo-covered.py`)

A Python extension for the **Nemo** file manager that reads `user.covered`
from the FUSE-mounted filesystem, adds emblems and sets coloured folder icons
via the same `metadata::custom-icon` mechanism used by `nemo-folder-color-switcher`.

| xattr value | Emblem | Folder colour | Mint-X icon theme |
|---|---|---|---|
| `covered`   | `emblem-default` (green)      | green  | `Mint-X` (default) |
| `uncovered` | `emblem-important` (red)      | red    | `Mint-X-Red`       |
| `partial`   | `emblem-new` (orange)         | orange | `Mint-X-Orange`    |
| `empty`     | *(none)*                      | aqua   | `Mint-X-Aqua`      |
| `error`     | `emblem-unreadable` (crossed) | grey   | `Mint-X-Grey`      |

Folder coloring reads `/usr/share/folder-color-switcher/colors.d/*.json` at
startup to find the color-variant icon theme for the active GTK theme.
`metadata::custom-icon` is set on the directory's GIO location — the same
mechanism used by `nemo-folder-color-switcher`.  
If the active theme has no colour variants (no entry in colors.d), only
emblems are shown; the folder icon falls back to the theme default.

# Installation

```sh
sudo cp nemo-extension/nemo-covered.py /usr/share/nemo-python/extensions/
nemo -q && nemo
```

Then navigate to the FUSE mount point in Nemo to see the color-coded emblems.

# Details

**Notes** This project was also an opportunity for me to test full code generation by LLM agent.

Most (all?) the code in this repo was generated. I did some manual adjustement, but in general tried to have a mechanism (via skills, doc and README) to have good enough generation handling completly by the (different) agents.

I tried different agents and technics to reduce the generation costs.

The code should be good enougth to work, and algorithmicly sound enough to cover the desinged usage.

## DB

For each scanned filesystem folder, a dedicated output directory is created
(e.g. `covered_home_yves/`). Inside that directory you will find:

| File | Purpose |
|---|---|
| `config.json` | Small JSON file with the scanned root path and device: `{"root":"/absolute/path", "device":1234}`. Useful for external scripts and for reconstructing filesystem paths during matching. |
| `filesize.db` | SQLite database containing the directory tree, file metadata, and the final `covered` flag. |
| `hash.db` | SQLite database caching blake3 hashes per inode (head hash and full hash). Kept separate so the match phase can be re-run or resumed incrementally. |

### `config.json`
```json
{"root":"/home/yves","device":2049}
```

### `filesize.db` tables

#### `dirs`
| Column | Type | Description |
|---|---|---|
| `inode` | INTEGER PRIMARY KEY | Directory inode |
| `parent_inode` | INTEGER | Parent directory inode (`NULL` for root) |
| `name` | TEXT | Directory basename |
| `covered` | INTEGER DEFAULT 0 | `0`=uncovered, `1`=covered, `2`=partial, `3`=empty, `4`=error — computed by `cover_report` |
| `error` | INTEGER DEFAULT 0 | `1` if directory could not be scanned (permission denied, etc.) |

#### `files`
| Column | Type | Description |
|---|---|---|
| `dir_inode` | INTEGER | Containing directory inode (part of PK) |
| `name` | TEXT | File basename (part of PK) |
| `inode` | INTEGER | File inode |
| `size` | INTEGER | File size in bytes |
| `mtime` | INTEGER | Modification time (seconds since epoch) |
| `covered` | INTEGER DEFAULT 0 | `1` if a matching file exists in backup, `0` otherwise |
| `error` | INTEGER DEFAULT NULL | `1` if file could not be accessed during match (permission denied, etc.) |
| `backup_id` | INTEGER DEFAULT NULL | Foreign key to `backup_db.id` identifying which backup matched this file (`NULL` if unmatched) |

Indexes: `idx_files_inode(inode)`, `idx_files_size(size)`

#### `backup_db`
| Column | Type | Description |
|---|---|---|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | Unique identifier for each backup source |
| `path` | TEXT NOT NULL UNIQUE | Absolute path of the backup DB |

This table tracks which backups have been matched against this source.
Rows are inserted automatically by `covered_match`; matching the same backup
again reuses the existing `id` (idempotent). The `backup_id` on each matched
file points to the first backup that contained a matching copy.

### `hash.db` tables

#### `hashes`
| Column | Type | Description |
|---|---|---|
| `inode` | INTEGER PRIMARY KEY | File inode |
| `head_hash` | BLOB | blake3 hash of the first 2048 bytes (`NULL` until computed) |
| `full_hash` | BLOB | blake3 hash of the entire file (`NULL` until computed) - **Indexed** |

Both hash columns are populated lazily during the match phase and cached for reuse.