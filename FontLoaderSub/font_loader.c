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

    vec_init(&c->loaded_font, sizeof(FL_FontMatch), alloc);
    str_db_init(&c->sub_font, alloc, 0, 1);
    str_db_init(&c->font_path, alloc, 0, 0);
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
  vec_free(&c->loaded_font);
  str_db_free(&c->sub_font);
  str_db_free(&c->font_path);
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
  int r;
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

static int
fl_walk_font_callback(const wchar_t *path, WIN32_FIND_DATA *data, void *arg) {
  FL_LoaderCtx *c = arg;
  if (WaitForSingleObject(c->event_cancel, 0) != WAIT_TIMEOUT)
    return FL_OS_ERROR;

  const size_t len = ass_strlen(path);
  const wchar_t *ext = path + len - 4;
  const int match_attr =
      !(data->dwFileAttributes &
        (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY));
  const int match_ext = (len > 4) && (ass_strncasecmp(ext, L".ttc", 4) == 0 ||
                                      ass_strncasecmp(ext, L".otf", 4) == 0 ||
                                      ass_strncasecmp(ext, L".ttf", 4) == 0);
  if (!(match_attr && match_ext))
    return FL_OK;

  // try load the file
  memmap_t map;
  FlMemMap(path, &map);
  if (map.data) {
    const wchar_t *tag = path + str_db_tell(&c->font_path) + 1;
    fs_add_font(c->font_set, tag, map.data, map.size);
  }
  return FL_OK;
}

int fl_scan_fonts(FL_LoaderCtx *c, const wchar_t *path, const wchar_t *cache) {
  // caller: fl_unload_fonts

  // free previous font set
  fs_free(c->font_set);
  c->font_set = NULL;

  int r = FlResolvePath(path, &c->font_path);
  // if path points to a file, find its parent directory
  if (1) {
    HANDLE test = CreateFile(
        str_db_get(&c->font_path, 0), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (test != INVALID_HANDLE_VALUE) {
      const size_t pos = FlPathParent(&c->font_path);
      if (pos) {
        str_db_seek(&c->font_path, pos - 1);
        wchar_t *buf = (wchar_t *)str_db_get(&c->font_path, 0);
        buf[pos - 1] = 0;
      }
      CloseHandle(test);
    }
  }

  if (cache) {
    if (r == FL_OK) {
      // load from cache
      str_db_seek(&c->walk_path, 0);
      if (str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0) &&
          str_db_push_u16_le(&c->walk_path, L"\\", 1) &&
          str_db_push_u16_le(&c->walk_path, cache, 0)) {
        // ok
      } else {
        r = FL_OUT_OF_MEMORY;
      }
    }
    if (r == FL_OK) {
      r = fs_cache_load(str_db_get(&c->walk_path, 0), c->alloc, &c->font_set);
    }
  } else {
    // search font files
    if (r == FL_OK) {
      str_db_seek(&c->walk_path, 0);
      if (!str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0))
        r = FL_OUT_OF_MEMORY;
    }
    if (r == FL_OK) {
      r = fs_create(c->alloc, &c->font_set);
    }
    if (r == FL_OK) {
      r = FlWalkDirStr(&c->walk_path, fl_walk_font_callback, c);
    }
  }
  if (r == FL_OK) {
    r = fs_build_index(c->font_set);
  }

  // failed
  if (r != FL_OK) {
    fs_free(c->font_set);
    c->font_set = NULL;
  }

  return r;
}

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache) {
  int r = FL_OK;
  str_db_seek(&c->walk_path, 0);
  if (str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0) &&
      str_db_push_u16_le(&c->walk_path, L"\\", 1) &&
      str_db_push_u16_le(&c->walk_path, cache, 0)) {
    // ok
  } else {
    r = FL_OUT_OF_MEMORY;
  }

  if (r == FL_OK) {
    r = fs_cache_dump(c->font_set, str_db_get(&c->walk_path, 0));
  }
  return r;
}

int fl_load_fonts(FL_LoaderCtx *c) {
  return FL_OS_ERROR;
}

int fl_unload_fonts(FL_LoaderCtx *c) {
  str_db_seek(&c->walk_path, 0);
  if (!str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0))
    return FL_OUT_OF_MEMORY;
  if (!str_db_push_u16_le(&c->walk_path, L"\\", 1))
    return FL_OUT_OF_MEMORY;

  const size_t pos = str_db_tell(&c->walk_path);
  FL_FontMatch *data = c->loaded_font.data;
  for (size_t i = 0; i != c->loaded_font.n; i++) {
    FL_FontMatch *m = &data[i];
    str_db_seek(&c->walk_path, pos);
    if (str_db_push_u16_le(&c->walk_path, m->filename, 0)) {
      const wchar_t *path = str_db_get(&c->walk_path, 0);
      RemoveFontResource(path);
    }
  }
  vec_clear(&c->loaded_font);

  return FL_OK;
}
