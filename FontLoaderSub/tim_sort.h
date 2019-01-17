#pragma once

#include "util.h"

typedef int (*Sort_Compare)(const void *a, const void *b, void *arg);

void tim_sort(
    void *ptr,
    size_t count,
    size_t size,
    allocator_t *alloc,
    Sort_Compare comp,
    void *arg);
