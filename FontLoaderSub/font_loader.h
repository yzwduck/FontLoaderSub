#pragma once

#include "cstl.h"
#include "util.h"
#include "font_set.h"

typedef struct {
  str_db_t sub_font;
  str_db_t db_path;
  FS_Set *font_set;

  void *event_cancel;
} FL_LoaderCtx;

int fl_init(FL_LoaderCtx *c, allocator_t *alloc);

int fl_free(FL_LoaderCtx *c);

int fl_cancel(FL_LoaderCtx *c);

int fl_add_subs(FL_LoaderCtx *c, const wchar_t *path[], size_t num);

int fl_load_fonts(FL_LoaderCtx *c, const wchar_t *path, const wchar_t *cache);

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache);
