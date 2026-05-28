# Nemo Extension (`nemo-covered.py`)

## Overview

`nemo-covered.py` is a Nemo (file manager) extension that visually enhances files
and folders exposed by the `cover_fuse` FUSE filesystem. It relies on the
`user.covered` extended attribute set on every FUSE inode.

**File**: `nemo-extension/nemo-covered.py`  
**Install path**: `/usr/share/nemo-python/extensions/nemo-covered.py`

## Features

### 1. Coloured Emblems

Every file and folder under the FUSE mount gets a standard emblem icon based on
its backup coverage state:

| `user.covered` value | Emblem | Meaning |
|----------------------|--------|---------|
| `covered` | emblem-default (green check) | File has a verified matching copy in at least one backup |
| `uncovered` | emblem-important (red exclamation) | File has no matching copy in any backup |
| `partial` | emblem-new (orange star) | Directory whose children are a mix of covered and uncovered |
| `empty` | *(none)* | Directory containing no files at all |
| `error` | emblem-unreadable (crossed circle) | An access error occurred during scanning |

### 2. Coloured Folder Icons

Directories get a colour-coded folder icon (via `metadata::custom-icon`) that
matches their coverage state. This uses the colour-variant icon themes from
`nemo-folder-color-switcher` (`/usr/share/folder-color-switcher/colors.d/`).

| State | Folder Colour |
|-------|--------------|
| covered | Green |
| uncovered | Red |
| partial | Orange |
| empty | Aqua |
| error | Grey |

### 3. Custom List Columns

Three additional columns are available in Nemo's list view (enable via
*View → Visible Columns* or right-click the column header):

| Column | Attribute | Visible for | Description |
|--------|-----------|-------------|-------------|
| **Covered** | `covered_state` | All items (files + dirs) | Shows the raw `user.covered` value: `covered`, `uncovered`, `partial`, `empty`, or `error` |
| **Backup** | `covered_backup` | Covered files only | Compact path to the backup root where a matching copy was found |
| **Covered at** | `covered_at` | Covered files only | Full absolute path of the matching file inside the backup |

### 4. Context Menu Items

Right-clicking any item under the FUSE mount shows:

#### "Open original containing folder"

- **Visible on**: All files and directories (regardless of coverage state)
- **Behaviour**:
  - For **files**: opens the source directory containing the original file
  - For **directories**: opens the source directory itself
- **Data source**: `user.covered_source` xattr (absolute path on the real filesystem)

#### "Open containing backup folder"

- **Visible on**: Covered files only (`user.covered` == `covered`)
- **Behaviour**: opens the backup directory containing the matching file
  (Nemo automatically selects/navigates to the file in the backup folder)
- **Data source**: `user.covered_at` xattr (absolute path inside the backup)

## Required xattrs

The extension reads the following extended attributes exposed by `cover_fuse`:

| xattr | Provided by | Used for |
|-------|-------------|----------|
| `user.covered` | All inodes | State string for emblems, folder colours, and the Covered column |
| `user.covered_source` | All files + dirs | Opening the original source folder |
| `user.covered_backup` | Covered files only | Backup path for the Backup column |
| `user.covered_at` | Covered files only | Full backup file path (Covered at column + context menu) |

## Install / Update

```bash
sudo cp nemo-extension/nemo-covered.py /usr/share/nemo-python/extensions/
nemo -q
```

## Dependencies

- **Python 3** with `gi` (GObject Introspection)
- **Nemo** file manager with Python extension support
- **nemo-folder-color-switcher** (optional — provides the colour-variant folder icons; without it, emblems and columns still work but folder icons won't be coloured)
- **cover_fuse** FUSE daemon running and mounted