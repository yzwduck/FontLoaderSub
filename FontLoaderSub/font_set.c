#include "font_set.h"

#include "cstl.h"
#include "ttf_parser.h"
#include "ass_string.h"
#include "tim_sort.h"
#include "util.h"

#define MAKE_TAG(a, b, c, d)                                             \
  ((uint32_t)(((uint8_t)(d) << 24)) | (uint32_t)(((uint8_t)(c) << 16)) | \
   (uint32_t)(((uint8_t)(b) << 8)) | (uint32_t)(((uint8_t)(a))))

#define KFontDbMagic (MAKE_TAG('f', 'l', 'd', 'c'))

struct _FS_Set {
  allocator_t *alloc;
  str_db_t db;
  FS_Stat stat;
  FS_Index *index;
  memmap_t map;
};

typedef struct {
  FS_Set *set;
  uint32_t id;
  size_t pos_ver;       // point to version
  size_t pos_face;      // point to first face name
  uint32_t count_face;  // number of discovered face name
  uint16_t last_lang_id;
} FS_ParseCtx;

typedef struct {
  uint32_t magic;
  FS_Stat stat;
  uint32_t size;
} FS_CacheHeader;

#define kTagVersion L"\tv:"
#define kTagVersionLen (3)
#define kTagError L"\t!!"
#define kTagErrorLen (3)

static int fs_parser_name_cb(
    uint32_t font_id,
    OTF_NameRecord *r,
    const wchar_t *str,
    void *arg) {
  FS_ParseCtx *c = (FS_ParseCtx *)arg;
  FS_Set *s = c->set;
  const uint32_t cch = be16(r->length) / sizeof str[0];

  if (font_id != c->id) {
    // new font
    c->id = font_id;
    c->pos_ver = str_db_tell(&s->db);
    c->pos_face = c->pos_ver;
    c->last_lang_id = 0;
  }

  if (cch == 0)
    return FL_OK;

  if (r->name_id == be16(5)) {
    // this is a version record
    if (c->last_lang_id == 0 || r->lang_id == be16(0x0409)) {
      // no previous version record, or encountered English version.
      // update/overwrite
      str_db_seek(&s->db, c->pos_ver);
      if (!str_db_push_prefix(&s->db, kTagVersion, kTagVersionLen))
        return FL_OUT_OF_MEMORY;
      const wchar_t *ver = str_db_push_u16_be(&s->db, str, cch);
      if (ver == NULL)
        return FL_OUT_OF_MEMORY;

      c->last_lang_id = r->lang_id;
      c->pos_face = str_db_tell(&s->db);
    }
  } else {
    // first, convert to little endian (and insert into database)
    const size_t pos_insert = str_db_tell(&s->db);
    const wchar_t *face = str_db_push_u16_be(&s->db, str, cch);
    if (face == NULL)
      return FL_OUT_OF_MEMORY;

    // check duplication
    const wchar_t *prev_face = str_db_str(&s->db, c->pos_face, face);
    if (prev_face == face) {
      ++c->count_face;
    } else {
      // duplicated, revert
      str_db_seek(&s->db, pos_insert);
    }
  }

  return FL_OK;
}

int fs_create(allocator_t *alloc, FS_Set **out) {
  int ok = 0;
  FS_Set *p = NULL;
  do {
    p = (FS_Set *)alloc->alloc(p, sizeof *p, alloc->arg);
    if (!p)
      break;
    str_db_init(&p->db, alloc, '\n', 2);

    p->alloc = alloc;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(p, 0, alloc->arg);
    p = NULL;
  }
  *out = p;
  return ok ? FL_OK : FL_OUT_OF_MEMORY;
}

int fs_free(FS_Set *s) {
  if (s) {
    allocator_t *alloc = s->alloc;
    str_db_free(&s->db);
    FlMemUnmap(&s->map);
    alloc->alloc(s, 0, alloc->arg);
  }
  return FL_OK;
}

int fs_stat(FS_Set *s, FS_Stat *stat) {
  if (s) {
    *stat = s->stat;
  }
  return 0;
}

int fs_add_font(FS_Set *s, const wchar_t *tag, void *buf, size_t size) {
  int ok = 0, r;
  str_db_t *db = &s->db;
  const size_t pos_filename = str_db_tell(db);
  if (str_db_push_u16_le(db, tag, 0) == NULL)
    return FL_OUT_OF_MEMORY;
  const size_t pos_db = str_db_tell(db);

  FS_ParseCtx ctx;
  do {
    ctx = (FS_ParseCtx){.set = s, .pos_ver = pos_db, .pos_face = pos_db};
    r = ttc_parse(buf, size, fs_parser_name_cb, &ctx);
    if (r == FL_OK && ctx.count_face > 0) {
      ok = 1;
      break;
    }

    str_db_seek(db, pos_db);
    ctx = (FS_ParseCtx){.set = s, .pos_ver = pos_db, .pos_face = pos_db};
    r = otf_parse(buf, size, fs_parser_name_cb, &ctx);
    if (r == FL_OK && ctx.count_face > 0) {
      ok = 1;
      break;
    }
  } while (0);

  if (ok) {
    if (!str_db_push_u16_le(db, L"", 0)) {
      r = FL_OUT_OF_MEMORY;
      ok = 0;
    }
  }

  s->stat.num_file++;
  if (ok) {
    s->stat.num_face += ctx.count_face;
  } else {
    // try preserve error message
    str_db_seek(db, pos_db);
    const wchar_t *m1 = str_db_push_u16_le(db, kTagError, kTagErrorLen);
    const wchar_t *m2 = str_db_push_u16_le(db, L"", 0);
    FlBreak();
    if (!(m1 && m2)) {
      // completely rollback
      str_db_seek(db, pos_filename);
      s->stat.num_file--;
    }
  }
  return r;
}

int fs_idx_comp(const void *pa, const void *pb, void *arg) {
  // FS_Set *s = arg;
  const FS_Index *a = pa, *b = pb;
  int cmp = FlStrCmpIW(a->face, b->face);

  if (cmp == 0) {
    cmp = FlVersionCmp(a->ver, b->ver);
  }

  return cmp;
}

int fs_build_index(FS_Set *s) {
  allocator_t *alloc = s->alloc;
  const size_t idx_size = s->stat.num_face * sizeof s->index[0];
  FS_Index *idx = (FS_Index *)alloc->alloc(s->index, idx_size, alloc->arg);
  s->index = idx;
  if (idx == NULL) {
    return s->stat.num_face ? FL_OUT_OF_MEMORY : FL_OK;
  }

  // for checking only
  FS_Stat stat = {.num_face = 0, .num_file = 0};

  int err = 0;
  int has_filename = 0;
  const wchar_t *line;
  size_t pos = 0;
  FS_Index last_idx = {0};
  while (!err && (line = str_db_next(&s->db, &pos)) != NULL) {
    if (line[0] == 0) {
      // empty line
      last_idx = (FS_Index){0};
      has_filename = 0;
    } else if (ass_strncmp(line, kTagVersion, kTagVersionLen) == 0) {
      // update version
      last_idx.ver = line + kTagVersionLen;
    } else if (ass_strncmp(line, kTagError, kTagErrorLen) == 0) {
      // ignore
    } else if (!has_filename) {
      // update filename
      last_idx.tag = line;
      has_filename = 1;
      stat.num_file++;
    } else {
      // face
      if (stat.num_face == s->stat.num_face) {
        err = 1;
        break;
      }
      last_idx.face = line;
      idx[stat.num_face++] = last_idx;
    }
  }

  if (stat.num_face != s->stat.num_face || stat.num_file != s->stat.num_file)
    err = 1;

  if (!err) {
    // sort
    tim_sort(idx, stat.num_face, sizeof idx[0], s->alloc, fs_idx_comp, s);
  } else {
    alloc->alloc(idx, 0, alloc->arg);
    idx = NULL;
    s->index = idx;
  }

  return err ? FL_OUT_OF_MEMORY : FL_OK;
}

int fs_iter_new(FS_Set *s, const wchar_t *face, FS_Iter *it) {
  uint32_t a = 0, b = s->stat.num_face - 1;
  uint32_t m = 0;
  if (s->index != NULL && s->stat.num_face != 0 && it != NULL) {
    while (a <= b) {
      m = a + (b - a) / 2;
      const wchar_t *got = s->index[m].face;
      const int t = FlStrCmpIW(face, got);
      if (t == 0) {
        a = b = m;
        break;
      }
      if (t > 0) {
        a = m + 1;
      } else {
        b = m - 1;
      }
    }
  }
  if (a != b || a != m) {
    // *it = (FS_Iter){0};
    it->set = NULL;
    it->query_id = 0;
    it->index_id = 0;
    return 0;
  } else {
    // found, skip to first match
    while (m > 0 && FlStrCmpIW(face, s->index[m - 1].face) == 0) {
      m--;
    }
    // find the latest
    for (b = m + 1; b != s->stat.num_face; b++) {
      if (FlStrCmpIW(face, s->index[b].face) != 0)
        break;
      if (FlVersionCmp(s->index[m].ver, s->index[b].ver) < 0) {
        m = b;
      }
    }
    // result = m
    *it =
        (FS_Iter){.set = s, .query_id = m, .index_id = m, .info = s->index[m]};
    return 1;
  }
}

static size_t str_cmp_x(const wchar_t *a, const wchar_t *b) {
  size_t r;
  for (r = 0; a[r] == b[r] && a[r]; r++) {
    // nop;
  }
  return r;
}

int fs_iter_next(FS_Iter *it) {
  FS_Set *s = it->set;
  if (s == NULL)
    return 0;
  if (it->index_id == s->stat.num_face)
    return 0;
  it->index_id++;
  const wchar_t *face = s->index[it->query_id].face;
  const wchar_t *ver = s->index[it->query_id].ver;

  for (; it->index_id != s->stat.num_face; it->index_id++) {
    const wchar_t *got_face = s->index[it->index_id].face;
    const wchar_t *got_ver = s->index[it->index_id].ver;
    const size_t df = str_cmp_x(face, got_face);
    const size_t dv = str_cmp_x(ver, got_ver);
    if (face[df] != 0) {
      // match end
      break;
    }
    if (ver[dv] == 0 && got_ver[dv] == 0) {
      // match found
      it->info = s->index[it->index_id];
      return 1;
    }
  }
  // *it = (FS_Iter){0};
  it->set = NULL;
  return 0;
}

int fs_cache_load(const wchar_t *path, allocator_t *alloc, FS_Set **out) {
  int ok = 0, r;
  FS_Set *s = NULL;
  memmap_t map = {0};

  do {
    r = FlMemMap(path, &map);
    if (map.data == NULL) {
      r = FL_OS_ERROR;
      break;
    }

    r = FL_UNRECOGNIZED;
    FS_CacheHeader *head = map.data;
    if (head->magic != KFontDbMagic)
      break;
    if (head->size != map.size)
      break;
    if (head->size < 8)
      break;

    // ensure NUL terminated
    const wchar_t *buf_tail = (wchar_t *)((char *)map.data + head->size);
    if (buf_tail[-1] != 0 && buf_tail[-2] != 0)
      break;

    r = FL_OUT_OF_MEMORY;
    fs_create(alloc, &s);
    if (s == NULL)
      break;
    str_db_loads(
        &s->db, (const wchar_t *)&head[1],
        (head->size - sizeof head[0]) / sizeof(wchar_t), '\n');
    s->stat = head->stat;

    ok = 1;
  } while (0);

  if (!ok) {
    fs_free(s);
    FlMemUnmap(&map);
    s = NULL;
  } else {
    s->map = map;
  }

  *out = s;
  return ok ? FL_OK : r;
}

int fs_cache_dump(FS_Set *s, const wchar_t *path) {
  int ok = 0;
  DWORD flags = FILE_ATTRIBUTE_NORMAL;
  if (s->stat.num_file == 0)
    flags |= FILE_FLAG_DELETE_ON_CLOSE;

  HANDLE h = CreateFile(
      path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, flags, NULL);
  do {
    if (h == INVALID_HANDLE_VALUE)
      break;
    const wchar_t *buf = str_db_get(&s->db, 0);
    FS_CacheHeader head = {
        .magic = KFontDbMagic,
        .stat = s->stat,
        .size = sizeof head + str_db_tell(&s->db) * sizeof buf[0]};

    DWORD dw_out;
    if (!WriteFile(h, &head, sizeof head, &dw_out, NULL))
      break;
    if (!WriteFile(h, buf, str_db_tell(&s->db) * sizeof buf[0], &dw_out, NULL))
      break;
    ok = 1;
  } while (0);

  CloseHandle(h);
  return ok ? FL_OK : FL_OS_ERROR;
}
