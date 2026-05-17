#!/usr/bin/env python3
"""
Create a sample covered filesystem database that exercises ALL states
for testing FUSE and Nemo extension.
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
        PRIMARY KEY (dir_inode, name)
    ) WITHOUT ROWID
""")
conn.execute("CREATE INDEX idx_files_inode ON files(inode)")
conn.execute("CREATE INDEX idx_files_size  ON files(size)")

# Meta
conn.execute("INSERT INTO meta VALUES ('device', 2050)")
conn.execute("INSERT INTO meta VALUES ('root_path', 0)")  # stored separately in config.json

# ---- Directory tree ----
# inode  parent_inode  name           covered (will be computed by cover_report)
dirs_data = [
    (1,   None, ""),                # root → should become partial
    (2,   1,    "docs"),            # → covered
    (3,   1,    "media"),           # → uncovered
    (4,   1,    "cache"),           # → empty
    (5,   1,    "mixed"),           # → partial
    (6,   1,    "nested"),          # → partial (because of children)
    (7,   6,    "sub_covered"),     # → covered
    (8,   6,    "sub_uncovered"),   # → uncovered
    (9,   6,    "sub_empty"),       # → empty
    (10,  6,    "sub_partial"),     # → partial
    (11,  1,    "deep_nested"),     # → covered
    (12,  11,   "inner"),           # → covered
    (13,  12,   "deeper"),          # → covered
    (14,  1,    "empty_nested"),    # → empty (recursively empty)
    (15,  14,   "sub_empty_a"),     # → empty
    (16,  14,   "sub_empty_b"),     # → empty
]

conn.executemany(
    "INSERT INTO dirs (inode, parent_inode, name, covered) VALUES (?, ?, ?, 0)",
    [(d[0], d[1], d[2]) for d in dirs_data]
)

# ---- Files ----
# dir_inode  name           inode  size   mtime      covered
files_data = [
    # docs/ (inode 2) → all covered
    (2,  "readme.txt",    101,  1024,  1700000000, 1),
    (2,  "guide.txt",     102,  2048,  1700000001, 1),

    # media/ (inode 3) → all uncovered
    (3,  "song.mp3",      201,  5000000, 1700001000, 0),
    (3,  "movie.mkv",     202,  1500000000, 1700002000, 0),

    # cache/ (inode 4) → no files (empty)

    # mixed/ (inode 5) → partial
    (5,  "a.txt",         301,  100,   1700003000, 1),
    (5,  "b.txt",         302,  200,   1700003001, 0),

    # nested/ (inode 6) → has one file directly, plus subdirs
    (6,  "top_file.txt",  401,  50,    1700004000, 1),

    # nested/sub_covered/ (inode 7) → covered
    (7,  "alpha.dat",     501,  4096,  1700005000, 1),
    (7,  "beta.dat",      502,  8192,  1700005001, 1),

    # nested/sub_uncovered/ (inode 8) → uncovered
    (8,  "lost.txt",      601,  42,    1700006000, 0),

    # nested/sub_empty/ (inode 9) → no files

    # nested/sub_partial/ (inode 10) → partial
    (10, "found.txt",     701,  512,   1700007000, 1),
    (10, "missing.txt",   702,  256,   1700007001, 0),

    # deep_nested/inner/deeper/ (inode 13) → covered
    (13, "data.bin",      801,  16384, 1700008000, 1),
    (13, "info.json",     802,  128,   1700008001, 1),

    # empty_nested/ and subdirs → no files (all empty recursively)
]

conn.executemany(
    "INSERT INTO files (dir_inode, name, inode, size, mtime, covered) VALUES (?, ?, ?, ?, ?, ?)",
    files_data
)

conn.commit()

# Verify
print(f"\nDirectories created: {conn.execute('SELECT count(*) FROM dirs').fetchone()[0]}")
print(f"Files created:       {conn.execute('SELECT count(*) FROM files').fetchone()[0]}")
for row in conn.execute("SELECT name, covered FROM files ORDER BY dir_inode, name"):
    print(f"  file: {row[0]:20s} covered={row[1]}")

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
# Insert some dummy hashes for our files to make the DB look realistic
# blake3 hash is 32 bytes
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