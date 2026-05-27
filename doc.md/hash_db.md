Fixed `resolve_covered_at` in `src/fuse_main.cpp`. The implementation had 4 logical issues that caused `user.covered_at` to always return empty:

**Files changed:**

1. **`src/db.hpp`** — Added `find_inode_by_full_hash()` to `HashDatabase` for reverse lookup of inode by full_hash blob.

2. **`src/db.cpp`** — Implemented `find_inode_by_full_hash()` using the existing `idx_hashes_full_hash` index.

3. **`src/fuse_main.cpp`** — Five fixes in `resolve_covered_at`:

   - **Added `src_hash_` member** (`covered::HashDatabase`) to `CoverFs`, initialized from `<source_folder>/hash.db`. Replaces raw sqlite3 opening in step 1.

   - **Fixed trailing slash bug**: `src_folder_` now strips trailing `/` so `std::filesystem::path::parent_path()` correctly resolves to the parent directory rather than returning the folder itself. Without this, backup hash.db paths were computed as `<source_folder>/covered_<bkproot>/hash.db` instead of the correct sibling path.

   - **Step 1**: Uses `src_hash_.get_full_hash(node.db_inode)` instead of raw sqlite3.

   - **Step 2-3**: Uses already-opened `bck_db[node.backup_id]` HashDatabase and `find_inode_by_full_hash()` instead of re-deriving backup hash path from the hardcoded empty string `bkp_root=""` and opening raw sqlite3.

   - **Step 4-5**: Hash.db lookup returns a **file** inode, but `build_backup_path()` expects a **dir** inode. Added a filesize.db lookup to resolve the file's `dir_inode` before building the path, then appends the filename. Previous code passed the file inode directly to `build_backup_path()` which queries the `dirs` table and returned empty.

**Verified**: `getfattr` now shows `user.covered_at="/nfs/tronaut/mnt/BD/BD/mcomix-1.2.1.tar.bz2"` for the test file.