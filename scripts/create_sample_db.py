#!/usr/bin/env python3
"""
Create a sample covered filesystem database that exercises ALL states
for testing FUSE and Nemo extension.

Includes the normal states (covered, uncovered, partial, empty) plus:
  - A directory flagged as error because the scanner could not open it (empty_locked)
  - A file flagged as error inside a normal directory (corrupt.dat in mixed/)
  - A file flagged as error inside another normal dir (broken.txt in broken_files_dir/)
"""

import sqlite3
import json
import os
import sys

OUTPUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "/tmp/covered_sample"

os.makedirs(OUTPUT_DIR, exist_ok=True)

# ---- config.json ----
config = {"root": "/tmp/covered_root"}
with open(os.path.join(OUTPUT_DIR, "config.json"), "w") as f:
    json.dump(config, f)
print(f"Created {OUTPUT_DIR}/config.json -> {config}")

# ---- filesize.db ----
db_path = os.path.join(OUTPUT_DIR, "filesize.db")
if os.path.exists(db_path):
    os.unlink(db_path)

conn = sqlite3.connect(db_path)
conn.execute("PRAGMA journal_mode=WAL")
conn.execute("""
    CREATE TABLE meta (
        key   TEXT PRIMARY KEY,
        value INTEGER NOT NULL
    ) WITHOUT ROWID
""")
conn.execute("""
    CREATE TABLE dirs (
        inode        INTEGER NOT NULL,
        parent_inode INTEGER,
        name         TEXT    NOT NULL,
        covered      INTEGER NOT NULL DEFAULT 0,
        error        INTEGER DEFAULT NULL,
        PRIMARY KEY (inode)
    ) WITHOUT ROWID
""")
conn.execute("""
    CREATE TABLE files (
        dir_inode  INTEGER NOT NULL,
        name       TEXT    NOT NULL,
        inode      INTEGER NOT NULL,
        size       INTEGER NOT NULL,
        mtime      INTEGER NOT NULL,
        covered    INTEGER NOT NULL DEFAULT 0,
        error      INTEGER DEFAULT NULL,
        PRIMARY KEY (dir_inode, name)
    ) WITHOUT ROWID
""")
conn.execute("CREATE INDEX idx_files_inode ON files(inode)")
conn.execute("CREATE INDEX idx_files_size  ON files(size)")

# Meta
conn.execute("INSERT INTO meta VALUES ('device', 2050)")
conn.execute("INSERT INTO meta VALUES ('root_path', 0)")

# ---- Directory tree ----
# inode  parent_inode  name                covered (computed by cover_report)  error (dir scanner-level)
dirs_data = [
    (1,   None, ""),                        # root → partial
    (2,   1,    "docs"),                    # → covered
    (3,   1,    "media"),                   # → uncovered
    (4,   1,    "cache"),                   # → empty
    (5,   1,    "mixed"),                   # → partial (has error file corrupt.dat)
    (6,   1,    "nested"),                  # → partial (because of children)
    (7,   6,    "sub_covered"),             # → covered
    (8,   6,    "sub_uncovered"),           # → uncovered
    (9,   6,    "sub_empty"),               # → empty
    (10,  6,    "sub_partial"),             # → partial
    (11,  1,    "deep_nested"),             # → covered
    (12,  11,   "inner"),                   # → covered
    (13,  12,   "deeper"),                  # → covered
    (14,  1,    "empty_nested"),            # → empty (recursively)
    (15,  14,   "sub_empty_a"),             # → empty
    (16,  14,   "sub_empty_b"),             # → empty
    (17,  1,    "broken_files_dir"),        # → partial (has covered + error files)
    # ---- scanner-level error ----
    (18,  1,    "empty_locked"),            # → error (scanner could not open this dir)
]

# Insert dirs — only empty_locked (inode 18) gets error=1
conn.executemany(
    "INSERT INTO dirs (inode, parent_inode, name, covered, error) VALUES (?, ?, ?, 0, ?)",
    [(d[0], d[1], d[2], 1 if d[0] == 18 else None) for d in dirs_data]
)

# ---- Files ----
# dir_inode  name            inode  size   mtime      covered  error
files_data = [
    # docs/ (inode 2) → all covered
    (2,  "readme.txt",     101,  1024,  1700000000, 1,    None),
    (2,  "guide.txt",      102,  2048,  1700000001, 1,    None),

    # media/ (inode 3) → all uncovered
    (3,  "song.mp3",       201,  5000000, 1700001000, 0,    None),
    (3,  "movie.mkv",      202,  1500000000, 1700002000, 0,    None),

    # cache/ (inode 4) → no files (empty)

    # mixed/ (inode 5) → partial (one covered, one uncovered, one error file)
    (5,  "a.txt",          301,  100,   1700003000, 1,    None),
    (5,  "b.txt",          302,  200,   1700003001, 0,    None),
    (5,  "corrupt.dat",    303,  4096,  1700003002, 0,    1),     # error file

    # nested/ (inode 6) → one file directly, plus subdirs
    (6,  "top_file.txt",   401,  50,    1700004000, 1,    None),

    # nested/sub_covered/ (inode 7) → covered
    (7,  "alpha.dat",      501,  4096,  1700005000, 1,    None),
    (7,  "beta.dat",       502,  8192,  1700005001, 1,    None),

    # nested/sub_uncovered/ (inode 8) → uncovered
    (8,  "lost.txt",       601,  42,    1700006000, 0,    None),

    # nested/sub_empty/ (inode 9) → no files

    # nested/sub_partial/ (inode 10) → partial
    (10, "found.txt",      701,  512,   1700007000, 1,    None),
    (10, "missing.txt",    702,  256,   1700007001, 0,    None),

    # deep_nested/inner/deeper/ (inode 13) → covered
    (13, "data.bin",       801,  16384, 1700008000, 1,    None),
    (13, "info.json",      802,  128,   1700008001, 1,    None),

    # empty_nested/ and subdirs → no files

    # broken_files_dir/ (inode 17) → partial (covered + error file)
    (17, "ok_file.txt",    901,  64,    1700009000, 1,    None),  # covered
    (17, "broken.txt",     902,  128,   1700009001, 0,    1),     # error file

    # empty_locked/ (inode 18) → no files (scanner could not open it)
]

conn.executemany(
    "INSERT INTO files (dir_inode, name, inode, size, mtime, covered, error) VALUES (?, ?, ?, ?, ?, ?, ?)",
    files_data
)

conn.commit()

# Verify
print(f"\nDirectories created: {conn.execute('SELECT count(*) FROM dirs').fetchone()[0]}")
print(f"Files created:       {conn.execute('SELECT count(*) FROM files').fetchone()[0]}")
for row in conn.execute("SELECT d.name, f.name, f.covered, f.error FROM files f JOIN dirs d ON f.dir_inode = d.inode ORDER BY d.inode, f.name"):
    err_str = " [ERROR]" if row[3] else ""
    print(f"  {row[0]:20s} / {row[1]:20s}  covered={row[2]}{err_str}")
print("---")
for row in conn.execute("SELECT name, error FROM dirs WHERE error IS NOT NULL ORDER BY name"):
    print(f"  DIR {row[0]:20s}  ERROR (scanner could not open)")
print("---")
for row in conn.execute("SELECT d.name, f.name FROM files f JOIN dirs d ON f.dir_inode = d.inode WHERE f.error IS NOT NULL ORDER BY d.name, f.name"):
    print(f"  FILE {row[0]:20s} / {row[1]:20s}  ERROR (access failure during match)")

conn.close()
print(f"\nDatabase created: {db_path}")

# ---- hash.db (minimal, needed for consistency) ----
hash_path = os.path.join(OUTPUT_DIR, "hash.db")
if os.path.exists(hash_path):
    os.unlink(hash_path)

hc = sqlite3.connect(hash_path)
hc.execute("PRAGMA journal_mode=WAL")
hc.execute("""
    CREATE TABLE hashes (
        inode     INTEGER PRIMARY KEY,
        head_hash BLOB,
        full_hash BLOB
    ) WITHOUT ROWID
""")
dummy_head = b'\x00' * 32
dummy_full = b'\x01' * 32
for f in files_data:
    hc.execute("INSERT OR IGNORE INTO hashes (inode, head_hash, full_hash) VALUES (?, ?, ?)",
               (f[2], dummy_head, dummy_full))

hc.commit()
hc.close()
print(f"Hash DB created: {hash_path}")

print(f"\nNow run: ./build/cover_report {OUTPUT_DIR}/")
print(f"Then mount: ./build/cover_fuse {OUTPUT_DIR}/ <mountpoint>")