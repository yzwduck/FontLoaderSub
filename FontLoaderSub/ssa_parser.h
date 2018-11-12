#ifndef SSA_PARSER_H
#define SSA_PARSER_H

#include "util.h"

typedef int (*ssa_parse_font_cb_t)(const wchar_t *font, size_t cch, void *arg);

wchar_t *TextFileFromPath(const wchar_t *path, size_t *cch, allocator_t *alloc);

int AssParseFont(const wchar_t *str,
                 size_t cch,
                 ssa_parse_font_cb_t cb,
                 void *arg);

#endif
