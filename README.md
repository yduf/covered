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

