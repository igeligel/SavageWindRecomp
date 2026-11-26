// The save log's per-file gate (runtime/services/fs.c): whether a host path is one of the game's save-data files.
// During an autosave the game issues many small reads and writes to those files, so the answer is computed once when
// the file is opened (IFileSystem::OpenFile) and kept in the HostFile; a read or write then only reads that flag. The
// test is the per-operation strstr it replaces, unchanged: a path that contains "savedata".
#pragma once
#include <stdbool.h>
#include <string.h>

static inline bool fs_savedata_path(const char *path) {
    return strstr(path, "savedata") != NULL;
}
