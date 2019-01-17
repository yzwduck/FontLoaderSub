#pragma once

#include "util.h"
#include "cstl.h"

int FlResolvePath(const wchar_t *path, str_db_t *s);

size_t FlPathParent(str_db_t *path);

typedef int (
    *FL_FileWalkCb)(const wchar_t *path, WIN32_FIND_DATA *data, void *arg);

int FlWalkDir(
    const wchar_t *path,
    allocator_t *alloc,
    FL_FileWalkCb callback,
    void *arg);

int FlWalkDirStr(str_db_t *path, FL_FileWalkCb callback, void *arg);
