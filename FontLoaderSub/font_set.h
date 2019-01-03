#ifndef FONT_SET_H
#define FONT_SET_H

#include "util.h"

typedef struct _font_set_t font_set_t;

typedef struct {
  uint32_t num_files;
  uint32_t num_faces;
  uint32_t num_error;
} font_set_stat_t;

typedef struct {
  font_set_t *set;          // internal use
  uint32_t query_id;        // internal use
  uint32_t set_id;          // internal use
  uint32_t file_id;         // public, unique in set
  const wchar_t *filename;  // public
  const wchar_t *version;   // public
} font_iter_t;

int FontSetCreate(allocator_t *alloc, font_set_t **out);

int FontSetFree(font_set_t *set);

int FontSetAdd(font_set_t *set, const wchar_t *tag, void *buffer, size_t size);

int FontSetBuildIndex(font_set_t *set);

const wchar_t *FontSetLookup(font_set_t *set, const wchar_t *face);

void FontSetStat(font_set_t *set, font_set_stat_t *stat);

int FontSetLookupIter(font_set_t *set, const wchar_t *face, font_iter_t *iter);

int FontSetLookupIterNext(font_iter_t *iter);

const wchar_t *FontSetLookupFileId(font_set_t *set, uint32_t id);

// Windows related

int FontSetLoad(const wchar_t *path, allocator_t *alloc, font_set_t **out);

int FontSetDump(font_set_t *set, const wchar_t *path);

#endif
