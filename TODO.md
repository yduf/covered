# Feature

## Refactor FUSE
- [x] - [FUSE Filesystem Refactoring Tips](https://chatgpt.com/share/6a157d07-127c-83eb-94d7-876be5503adc)
  - [x] - backup DB column
  - [x] - covered at column
- [x] - FUSE media player support 
  - [x] - add skill
  - [x] - add probe tests

## Nemo extension
- [ ] - missing information
  - [x]] - covered column
  - [ ] - shortened covered column


## Other

- [ ] add a new state for protected folder (so that they don't appears empty)
 
- [ ] add update mode for backup / source
  - [ ] - general update that just rescan for new / deleted inode - preserving computed hash & covered
    - [ ] issue is just preserving covered state / compare to a regular scan - since hash are stored elsewhere

- [ ] - check if backup_at can be improved by joining db tables.

- add a treeview for size (like windirstat)
- make scan statistic and covered statisc be part of config.json

- make sure json is only provided as input and not consumed by tools
- => tools use DB?