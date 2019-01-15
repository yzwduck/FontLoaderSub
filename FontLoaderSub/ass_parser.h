#pragma once

#include <stddef.h>

typedef int (*ASS_FontCallback)(const wchar_t *font, size_t cch, void *arg);

void ass_process_data(
    const wchar_t *data,
    size_t cch,
    ASS_FontCallback cb,
    void *arg);
