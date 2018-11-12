#ifndef FONT_SET_H
#define FONT_SET_H

#include "util.h"

typedef struct _font_set_t font_set_t;

typedef struct {
  uint32_t num_files;
  uint32_t num_faces;
  uint32_t num_error;
} font_set_stat_t;

int FontSetCreate(allocator_t *alloc, font_set_t **out);

int FontSetFree(font_set_t *set);

int FontSetAdd(font_set_t *set, const wchar_t *tag, void *buffer, size_t size);

int FontSetBuildIndex(font_set_t *set);

const wchar_t *FontSetLookup(font_set_t *set, const wchar_t *face);

void FontSetStat(font_set_t *set, font_set_stat_t *stat);

// Windows related

int FontSetLoad(const wchar_t *path, allocator_t *alloc, font_set_t **out);

int FontSetDump(font_set_t *set, const wchar_t *path);

#endif
