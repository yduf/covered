# Covered

A Tool to check if I have a backup of my file somewhere.

Compare content of source & backup
and list file in source do not exist somewhere on backup.

For every file covered 
- highlight place where it exist
- highlitht duplicates on sources

## Principle

For source and backup, 
build a database of files

For filtering
- by size
  - by minihash

For garantee
- by Hash 

Then compare database for source & backup

And finally list results