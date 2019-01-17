#include "font_loader.h"

int fl_init(FL_LoaderCtx *c, allocator_t *alloc) {
  return FL_OS_ERROR;
}

int fl_free(FL_LoaderCtx *c) {
  return FL_OS_ERROR;
}

int fl_cancel(FL_LoaderCtx *c) {
  return FL_OS_ERROR;
}

int fl_add_subs(FL_LoaderCtx *c, const wchar_t *path[], size_t num) {
  return FL_OS_ERROR;
}

int fl_load_fonts(FL_LoaderCtx *c, const wchar_t *path, const wchar_t *cache) {
  return FL_OS_ERROR;
}

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache) {
  return FL_OS_ERROR;
}
