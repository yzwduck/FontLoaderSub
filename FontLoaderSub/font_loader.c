#include "font_loader.h"

#include <Windows.h>
#include <bcrypt.h>
#include "ass_string.h"
#include "ass_parser.h"
#include "path.h"
#include "mock_config.h"

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)

int fl_init(FL_LoaderCtx *c, allocator_t *alloc) {
  int r = FL_OK;
  *c = (FL_LoaderCtx){.alloc = alloc};

  do {
    vec_init(&c->loaded_font, sizeof(FL_FontMatch), alloc);
    str_db_init(&c->sub_font, alloc, 0, 1);
    str_db_init(&c->font_path, alloc, 0, 0);
    str_db_init(&c->walk_path, alloc, 0, 0);

    c->event_cancel = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!c->event_cancel) {
      r = FL_OS_ERROR;
      break;
    }
    const NTSTATUS status = BCryptOpenAlgorithmProvider(
        &c->hash_alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (!NT_SUCCESS(status)) {
      r = FL_OS_ERROR;
      break;
    }
  } while (0);

  if (r != FL_OK)
    fl_free(c);
  return r;
}

int fl_free(FL_LoaderCtx *c) {
  CloseHandle(c->event_cancel);
  BCryptCloseAlgorithmProvider(c->hash_alg, 0);
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

static int fl_check_cancel(FL_LoaderCtx *c) {
  if (WaitForSingleObject(c->event_cancel, 0) != WAIT_TIMEOUT)
    return FL_OS_ERROR;
  return FL_OK;
}

static int fl_sub_font_callback(const wchar_t *font, size_t cch, void *arg) {
  FL_LoaderCtx *c = arg;
  if (cch != 0) {
    if (font[0] == '@') {
      // skip prefix '@'
      font++;
      cch--;
    }
    if (cch == 0)
      return FL_OK;

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
  const int r = fl_check_cancel(c);
  if (r != FL_OK)
    return r;

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
  const int r = fl_check_cancel(c);
  if (r != FL_OK)
    return r;

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
    // skip the base path + '\'
    const wchar_t *tag = path + str_db_tell(&c->font_path) + 1;
    fs_add_font(c->font_set, tag, map.data, map.size);
    FlMemUnmap(&map);
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
      if (!str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0) ||
          !str_db_push_u16_le(&c->walk_path, L"\\", 1) ||
          !str_db_push_u16_le(&c->walk_path, cache, 0)) {
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
  if (!str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0) ||
      !str_db_push_u16_le(&c->walk_path, L"\\", 1) ||
      !str_db_push_u16_le(&c->walk_path, cache, 0)) {
    r = FL_OUT_OF_MEMORY;
  }

  if (r == FL_OK) {
    r = fs_cache_dump(c->font_set, str_db_get(&c->walk_path, 0));
  }
  return r;
}

static int CALLBACK enum_fonts(
    const LOGFONTW *lfp,
    const TEXTMETRICW *tmp,
    DWORD fontType,
    LPARAM lParam) {
  int *r = (int *)lParam;
  *r = 1;
  return 1;  // continue
}

static int IsFontInstalled(const wchar_t *face) {
  if (MOCK_NO_SYS)
    return 0;
  int found = 0;
  HDC dc = GetDC(0);
  EnumFontFamilies(dc, face, enum_fonts, (LPARAM)&found);
  ReleaseDC(0, dc);
  return found;
}

static int fl_face_loaded(FL_LoaderCtx *c, const wchar_t *face) {
  const size_t pos = str_db_tell(&c->walk_path);
  FL_FontMatch *data = c->loaded_font.data;
  for (size_t i = 0; i != c->loaded_font.n; i++) {
    FL_FontMatch *m = &data[i];
    if (m->face == face)
      return 1;
  }
  return 0;
}

static int fl_file_loaded(FL_LoaderCtx *c, const wchar_t *file) {
  const size_t pos = str_db_tell(&c->walk_path);
  FL_FontMatch *data = c->loaded_font.data;
  for (size_t i = 0; i != c->loaded_font.n; i++) {
    FL_FontMatch *m = &data[i];
    if (m->filename == file)
      return 1;
  }
  return 0;
}

static int fl_hash_loaded(FL_LoaderCtx *c, const uint8_t hash[32]) {
  const size_t pos = str_db_tell(&c->walk_path);
  FL_FontMatch *data = c->loaded_font.data;
  for (size_t i = 0; i != c->loaded_font.n; i++) {
    FL_FontMatch *m = &data[i];
    if (m->flag & FL_LOAD_OK) {
      uint8_t dif = 0;
      for (int j = 0; j != 32; j++) {
        dif |= m->hash[j] ^ hash[j];
      }
      if (!dif)
        return !!dif;
    }
  }
  return 0;
}

static int
fl_calc_hash(FL_LoaderCtx *c, const void *data, size_t size, uint8_t res[32]) {
  int ok = 0;
  NTSTATUS status;
  BCRYPT_HASH_HANDLE hash = NULL;
  void *hash_obj = NULL;
  DWORD sz_hash_obj = 0;
  DWORD sz_data = 0;
  allocator_t *alloc = c->alloc;

  do {
    status = BCryptGetProperty(
        c->hash_alg, BCRYPT_OBJECT_LENGTH, (PBYTE)&sz_hash_obj,
        sizeof sz_hash_obj, &sz_data, 0);
    if (!NT_SUCCESS(status))
      break;

    hash_obj = alloc->alloc(hash_obj, sz_hash_obj, alloc->arg);
    if (hash_obj == NULL)
      break;

    status =
        BCryptCreateHash(c->hash_alg, &hash, hash_obj, sz_hash_obj, NULL, 0, 0);
    if (!NT_SUCCESS(status))
      break;

    status = BCryptHashData(hash, (PBYTE)data, size, 0);
    if (!NT_SUCCESS(status))
      break;

    status = BCryptFinishHash(hash, res, 32, 0);
    if (!NT_SUCCESS(status))
      break;

    ok = 1;
  } while (0);

  BCryptDestroyHash(hash);
  alloc->alloc(hash_obj, 0, alloc->arg);
  return ok ? FL_OK : FL_OS_ERROR;
}

static int
fl_load_file(FL_LoaderCtx *c, const wchar_t *face, const wchar_t *file) {
  int r = FL_OK;
  memmap_t map = {0};
  uint8_t hash[32];

  do {
    if (vec_prealloc(&c->loaded_font, 1) == 0) {
      r = FL_OUT_OF_MEMORY;
      break;
    }

    // check 1: if file pointer is loaded
    if (fl_file_loaded(c, file)) {
      r = FL_DUP;
      break;
    }

    // check 2: hash
    str_db_seek(&c->walk_path, 0);
    if (!str_db_push_u16_le(&c->walk_path, str_db_get(&c->font_path, 0), 0) ||
        !str_db_push_u16_le(&c->walk_path, L"\\", 1) ||
        !str_db_push_u16_le(&c->walk_path, file, 0)) {
      r = FL_OUT_OF_MEMORY;
      break;
    }

    const wchar_t *full_path = str_db_get(&c->walk_path, 0);
    FlMemMap(full_path, &map);
    if (map.data == NULL) {
      r = FL_OS_ERROR;
      break;
    }

    r = fl_calc_hash(c, map.data, map.size, hash);
    if (r != FL_OK)
      break;

    if (fl_hash_loaded(c, hash)) {
      r = FL_DUP;
      break;
    }

    if (MOCK_FAKE_LOAD) {
      Sleep(50);
    } else if (AddFontResource(full_path) != 0) {
      r = FL_OS_ERROR;
      break;
    }
  } while (0);

  FlMemUnmap(&map);
  if (r != FL_OUT_OF_MEMORY && r != FL_DUP) {
    FL_FontMatch m;
    m.flag = (r == FL_OK) ? FL_LOAD_OK : FL_LOAD_ERR;
    m.face = face;
    m.filename = file;
    uint64_t *src = (uint64_t *)hash;
    uint64_t *dst = (uint64_t *)m.hash;
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = src[3];
    vec_append(&c->loaded_font, &m, 1);
  }
  return r;
}

int fl_load_fonts(FL_LoaderCtx *c) {
  // caller: fl_unload_fonts

  int r = FL_OK;

  // pass 1: scan for existing fonts
  size_t pos_it = 0;
  const wchar_t *face;
  while (r == FL_OK && (face = str_db_next(&c->sub_font, &pos_it)) != NULL) {
    if ((r = fl_check_cancel(c)) != FL_OK)
      return r;

    if (IsFontInstalled(face)) {
      if (vec_prealloc(&c->loaded_font, 1) == 0)
        r = FL_OUT_OF_MEMORY;
      if (r == FL_OK) {
        // FL_FontMatch m = {.flag = FL_OS_LOADED, .face = face};
        FL_FontMatch m;
        m.flag = FL_OS_LOADED;
        m.face = face;
        m.filename = NULL;
        vec_append(&c->loaded_font, &m, 1);
      }
    }
  }

  // pass 2: load the missing font
  const size_t sys_fonts = c->loaded_font.n;
  pos_it = 0;
  while (r != FL_OUT_OF_MEMORY &&
         (face = str_db_next(&c->sub_font, &pos_it)) != NULL) {
    if (fl_face_loaded(c, face))
      continue;
    if (vec_prealloc(&c->loaded_font, 1) == 0) {
      r = FL_OUT_OF_MEMORY;
      break;
    }

    FS_Iter it;
    if (!fs_iter_new(c->font_set, face, &it)) {
      // FL_FontMatch m = {.flag = FL_LOAD_MISS, .face = face};
      FL_FontMatch m;
      m.flag = FL_LOAD_MISS;
      m.face = face;
      m.filename = NULL;
      vec_append(&c->loaded_font, &m, 1);
    } else {
      int num_loaded = 0;
      int num_dup = 0;
      int num_total = 0;
      do {
        if ((r = fl_check_cancel(c)) != FL_OK)
          return r;

        r = fl_load_file(c, face, it.info.tag);
        num_total++;
        if (r == FL_DUP)
          num_dup++;
        if (r == FL_OK)
          num_loaded++;
      } while (r != FL_OUT_OF_MEMORY && num_loaded <= 16 && fs_iter_next(&it));
      if (num_dup == num_total) {
        // FL_FontMatch m = {.flag = FL_LOAD_DUP, .face = face};
        FL_FontMatch m;
        m.flag = FL_LOAD_DUP;
        m.face = face;
        m.filename = NULL;
        vec_append(&c->loaded_font, &m, 1);
      }
    }
  }

  return r;
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
    if (m->filename && str_db_push_u16_le(&c->walk_path, m->filename, 0)) {
      const wchar_t *path = str_db_get(&c->walk_path, 0);
      RemoveFontResource(path);
    }
  }
  vec_clear(&c->loaded_font);

  return FL_OK;
}
