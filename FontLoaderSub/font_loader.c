#include "font_loader.h"

#include <Windows.h>
#include "ass_string.h"
#include "ass_parser.h"
#include "path.h"

int fl_init(FL_LoaderCtx *c, allocator_t *alloc) {
  int r;
  *c = (FL_LoaderCtx){.alloc = alloc};

  do {
    r = fs_create(alloc, &c->font_set);
    if (r != FL_OK)
      break;

    str_db_init(&c->sub_font, alloc, 0, 1);
    str_db_init(&c->db_path, alloc, 0, 0);
    str_db_init(&c->walk_path, alloc, 0, 0);

    c->event_cancel = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!c->event_cancel) {
      r = FL_OS_ERROR;
    }
  } while (0);

  if (r != FL_OK)
    fl_free(c);
  return r;
}

int fl_free(FL_LoaderCtx *c) {
  CloseHandle(c->event_cancel);
  str_db_free(&c->sub_font);
  str_db_free(&c->db_path);
  str_db_free(&c->walk_path);
  fs_free(c->font_set);

  return FL_OK;
}

int fl_cancel(FL_LoaderCtx *c) {
  return SetEvent(c->event_cancel) ? FL_OK : FL_OS_ERROR;
}

static int fl_sub_font_callback(const wchar_t *font, size_t cch, void *arg) {
  FL_LoaderCtx *c = arg;
  if (cch != 0) {
    if (font[0] == '@') {
      // skip prefix '@'
      font++;
      cch--;
    }

    const size_t pos = str_db_tell(&c->sub_font);
    const wchar_t *insert = str_db_push_u16_le(&c->sub_font, font, cch);
    if (insert == NULL) {
      return FL_OUT_OF_MEMORY;
    }

    const wchar_t *match = str_db_str(&c->sub_font, 0, insert);
    if (match == insert) {
      // not duplicated
      c->num_sub_font++;
    } else {
      str_db_seek(&c->sub_font, pos);
    }
  }
  return FL_OK;
}

static int
fl_walk_sub_callback(const wchar_t *path, WIN32_FIND_DATA *data, void *arg) {
  FL_LoaderCtx *c = arg;
  if (WaitForSingleObject(c->event_cancel, 0) != WAIT_TIMEOUT)
    return FL_OS_ERROR;

  const size_t len = ass_strlen(path);
  const wchar_t *ext = path + len - 4;
  const int match_attr =
      !(data->dwFileAttributes &
        (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY));
  const int match_size =
      (data->nFileSizeHigh == 0 && data->nFileSizeLow <= 64 * 1024 * 1024);
  const int match_ext = (len > 4) && (ass_strncasecmp(ext, L".ass", 4) == 0 ||
                                      ass_strncasecmp(ext, L".ssa", 4) == 0);
  if (!(match_attr && match_size && match_ext))
    return FL_OK;

  memmap_t map;
  wchar_t *content = NULL;
  size_t cch = 0;
  do {
    FlMemMap(path, &map);
    if (!map.data)
      break;
    content = FlTextDecode(map.data, map.size, &cch, c->alloc);
    if (content == NULL)
      break;

    c->num_sub++;
    ass_process_data(content, cch, fl_sub_font_callback, c);
  } while (0);

  FlMemUnmap(&map);
  c->alloc->alloc(content, 0, c->alloc->arg);

  return FL_OK;
}

int fl_add_subs(FL_LoaderCtx *c, const wchar_t *path) {
  int r = FL_OK;
  do {
    str_db_seek(&c->walk_path, 0);
    r = FlResolvePath(path, &c->walk_path);
    if (r == FL_OS_ERROR) {
      // ignore error
      r = FL_OK;
      break;
    } else if (r != FL_OK) {
      break;
    }

    r = FlWalkDirStr(&c->walk_path, fl_walk_sub_callback, c);
    if (r != FL_OK)
      break;
  } while (0);
  return r;
}

int fl_load_fonts(FL_LoaderCtx *c, const wchar_t *path, const wchar_t *cache) {
  return FL_OS_ERROR;
}

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache) {
  return FL_OS_ERROR;
}
