#include "font_set.h"

#include <Windows.h>
#include <stdio.h>

// internal

#define MAKE_TAG(a, b, c, d)                                             \
  ((uint32_t)(((uint8_t)(d) << 24)) | (uint32_t)(((uint8_t)(c) << 16)) | \
   (uint32_t)(((uint8_t)(b) << 8)) | (uint32_t)(((uint8_t)(a))))

#define kTagVersion L"\tv:"

typedef enum FONT_TAG {
  FONT_TAG_TTCF = MAKE_TAG('t', 't', 'c', 'f'),
  FONT_TAG_OTTO = MAKE_TAG('O', 'T', 'T', 'O'),
  FONT_TAG_NAME = MAKE_TAG('n', 'a', 'm', 'e'),
  FONT_DB_TAG_MAGIC = MAKE_TAG('f', 'l', 'd', 'c')
} FONT_TAG;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint16_t major_ver;
  uint16_t minor_ver;
  uint32_t num_fonts;
} ttc_header_t;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint16_t num_tables;
  uint16_t search_range;
  uint16_t entry_selector;
  uint16_t range_shift;
} otf_header_t;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint32_t checksum;
  uint32_t offset;
  uint32_t length;
} otf_header_record_t;

typedef struct {
  uint16_t format;
  uint16_t count;
  uint16_t offset;
} otf_name_header_t;

typedef struct {
  uint16_t platform;
  uint16_t encoding;
  uint16_t lang_id;
  uint16_t name_id;
  uint16_t length;  // bytes for string
  uint16_t offset;  // to string, from otf_name_header::offset
} otf_name_record_t;

typedef enum OTF_PLATFORM {
  OTF_PLATFORM_UNICODE = 0,
  OTF_PLATFORM_WINDOWS = 3
} OTF_PLATFORM;

typedef int (*font_name_cb_t)(otf_name_record_t *r,
                              void *str_buffer,
                              void *arg);

static int IsInterestedNameId(uint16_t name_id) {
  switch (name_id) {
  case 1:  // Font Family name
  case 4:  // Full font name
    return 1;
  case 0:   // Copyright notice
  case 2:   // Font Subfamily name
  case 3:   // Unique font identifier
  case 5:   // Version string
  case 6:   // PostScript name for the font
  case 7:   // Trademark
  case 8:   // Manufacturer Name
  case 9:   // Designer
  case 10:  // Description
  case 11:  // URL Vendor
  case 12:  // URL Designer
  case 13:  // License Description
  case 14:  // License Info URL
  case 15:  // Reserved
  case 16:  // Typographic Family name
  case 17:  // Typographic Subfamily name
  case 18:  // Compatible Full (Macintosh only)
  case 19:  // Sample text
  case 20:  // PostScript CID find font name
  case 21:  // WWS Family Name
  case 22:  // WWS Subfamily Name
  case 23:  // Light Background Palette
  case 24:  // Dark Background Palette
  case 25:  // Variations PostScript Name Prefix
  default:
    return 0;
  }
}

static int FontParseTableName(const char *buffer,
                              const char *eos,
                              font_name_cb_t cb,
                              void *arg) {
  otf_name_header_t *head = (otf_name_header_t *)buffer;
  if (buffer + sizeof *head > eos)
    return FL_CORRUPTED;
  // extract header
  const uint16_t format = be16(head->format);
  const uint16_t count = be16(head->count);
  const char *str_buffer = buffer + be16(head->offset);
  otf_name_record_t *records = (otf_name_record_t *)(head + 1);

  // range check for records
  if (buffer + sizeof *head + count * sizeof records[0] > eos)
    return FL_CORRUPTED;
  // range check for all strings
  for (uint16_t i = 0; i != count; i++) {
    otf_name_record_t *r = &records[i];
    if (str_buffer + be16(r->offset) + be16(r->length) > eos)
      return FL_CORRUPTED;
  }

  int ret = 0;

  // loop & callback for Version
  for (uint16_t i = 0; i != count; i++) {
    otf_name_record_t *r = &records[i];
    // filter version string (name_id == 5)
    if (r->name_id == be16(5) && r->platform == be16(OTF_PLATFORM_WINDOWS)) {
      wchar_t *str_be = (wchar_t *)(str_buffer + be16(r->offset));
      // fire callback
      ret = cb(r, str_be, arg);
      if (ret != FL_OK)
        return ret;
    }
  }

  // loop & callback for each record
  for (uint16_t i = 0; i != count; i++) {
    otf_name_record_t *r = &records[i];
    // filter
    if (IsInterestedNameId(be16(r->name_id)) &&
        r->platform == be16(OTF_PLATFORM_WINDOWS)) {
      wchar_t *str_be = (wchar_t *)(str_buffer + be16(r->offset));
      // fire callback
      ret = cb(r, str_be, arg);
      if (ret != FL_OK)
        return ret;
    }
  }
  return FL_OK;
}

static int FontParseOTF(const char *buffer,
                        const char *eos,
                        const char *start,
                        font_name_cb_t cb,
                        void *arg) {
  if (start == NULL)
    start = buffer;
  otf_header_t *head = (otf_header_t *)buffer;
  if (buffer + sizeof *head > eos)
    return FL_UNRECOGNIZED;
  if (head->tag != FONT_TAG_OTTO && head->tag != be32(0x00010000))
    return FL_UNRECOGNIZED;
  const uint16_t num_tables = be16(head->num_tables);
  otf_header_record_t *record = (otf_header_record_t *)(head + 1);
  if (buffer + sizeof *head + num_tables * sizeof record[0] > eos)
    return FL_CORRUPTED;
  for (uint16_t i = 0; i != num_tables; i++) {
    if (record[i].tag == FONT_TAG_NAME) {
      int a = 0;
      const char *ptr = start + be32(record[i].offset);
      const uint32_t length = be32(record[i].length);
      if (ptr + length > eos)
        return FL_CORRUPTED;
      FontParseTableName(ptr, ptr + length, cb, arg);
    }
  }
  return FL_OK;
}

static int FontParseTTC(const char *buffer,
                        const char *eos,
                        font_name_cb_t cb,
                        void *arg) {
  ttc_header_t *head = (ttc_header_t *)buffer;
  const size_t size = eos - buffer;
  if (size < sizeof *head)
    return FL_UNRECOGNIZED;
  if (head->tag != FONT_TAG_TTCF)
    return FL_UNRECOGNIZED;

  const uint32_t num_fonts = be32(head->num_fonts);
  const uint32_t *offset = (uint32_t *)(head + 1);
  if (size < (sizeof *head) + sizeof(offset[0]) * num_fonts)
    return FL_CORRUPTED;
  for (uint32_t i = 0; i != num_fonts; i++) {
    do {
      const char *ptr = buffer + be32(offset[i]);
      if (ptr > eos)
        break;
      int r = FontParseOTF(ptr, eos, buffer, cb, arg);
      if (r == FL_UNRECOGNIZED)
        r = FL_CORRUPTED;
      if (r != FL_OK)
        return r;
    } while (0);
  }
  return FL_OK;
}

typedef struct {
  str_db_t *db;
  uint32_t start;
  int count;
  uint16_t last_lang_id;
} font_file_parse_t;

static int FontNameVersionCb(otf_name_record_t *rec,
                             wchar_t *str_be,
                             uint32_t cch,
                             font_file_parse_t *c) {
  if (rec->name_id != be16(5)) {
    if (c->last_lang_id != 0) {
      // version locked
      c->start = StrDbTell(c->db);
      c->last_lang_id = 0;
    }
    return 0;
  }
  // version found
  if (c->last_lang_id == 0 || rec->lang_id == be16(0x0409)) {
    if (c->last_lang_id != 0)
      StrDbRewind(c->db, c->start);
    else
      c->start = StrDbTell(c->db);
    c->last_lang_id = rec->lang_id;
    StrDbPushPrefix(c->db, kTagVersion, 3);
    StrDbPushU16be(c->db, str_be, cch);
  }
  return 1;
}

static int FontNameCb2(otf_name_record_t *rec, void *str_buf, void *arg) {
  int r;
  font_file_parse_t *c = (font_file_parse_t *)arg;
  const uint32_t pos = StrDbTell(c->db);
  wchar_t *str_be = (wchar_t *)str_buf;
  uint32_t cch = be16(rec->length) / sizeof(str_be[0]);
  if (cch > 0) {
    if (FontNameVersionCb(rec, str_be, cch, c))
      return FL_OK;
    r = StrDbPushU16be(c->db, str_be, cch);
    if (r != FL_OK)
      return r;

    wchar_t *utf16 = &c->db->buffer[pos];
    if (StrDbIsDuplicate(c->db, c->start, pos)) {
      StrDbRewind(c->db, pos);
    } else {
      // ok
      c->count++;
      // wprintf(L"  %d.%s\n", c->count, utf16);
      int stop = 0;
    }
  }
  return FL_OK;
}

typedef struct {
  uint32_t face;
  uint32_t tag;
  uint32_t ver;
} font_pair_t;

static int FontPairCmp(wchar_t *base, font_pair_t *a, font_pair_t *b) {
  const wchar_t *face_a = base + a->face;
  const wchar_t *face_b = base + b->face;
  int cmp = FlStrCmpIW(face_a, face_b);

  if (cmp == 0) {
    if (b->ver == (uint32_t)-1)
      cmp = 1;
    else if (a->ver == (uint32_t)-1)
      cmp = -1;
    else {
      const wchar_t *ver_a = base + a->ver;
      const wchar_t *ver_b = base + b->ver;
      cmp = FlVersionCmp(ver_a, ver_b);
      if (0 && cmp == 0) {
        const wchar_t *path_a = base + a->tag;
        const wchar_t *path_b = base + b->tag;
        cmp = FlStrCmpIW(path_a, path_b);
      }
    }
  }

  return cmp;
}

static void FontSetQSort(font_pair_t *p, wchar_t *base, int low, int high) {
  if (low >= high)
    return;
  int key = low, a = low, b = high;
  font_pair_t tmp = p[low];

  while (low < high) {
    while (low < high && FlStrCmpIW(base + p[high].face, base + tmp.face) >= 0)
      --high;
    p[low] = p[high];
    while (low < high && FlStrCmpIW(base + p[low].face, base + tmp.face) <= 0)
      ++low;
    p[high] = p[low];
  }
  p[low] = tmp;
  if (a < low)
    FontSetQSort(p, base, a, low - 1);
  if (low < b)
    FontSetQSort(p, base, low + 1, b);
}

static void FontSetSelectSort(font_pair_t *p,
                              wchar_t *base,
                              uint32_t low,
                              uint32_t high) {
  for (uint32_t i = low; i < high; i++) {
    uint32_t m = i;
    for (uint32_t j = i + 1; j != high; j++) {
      if (FontPairCmp(base, &p[m], &p[j]) > 0)
        // if (FlStrCmpIW(base + p[m].face, base + p[j].face) > 0)
        m = j;
    }
    if (m != i) {
      const font_pair_t tmp = p[i];
      p[i] = p[m];
      p[m] = tmp;
    }
  }
}

static void FontSetTimSortI(font_pair_t *p,
                            font_pair_t *ex,
                            wchar_t *base,
                            uint32_t low,
                            uint32_t high) {
  if (low >= high)
    return;
  if (1 && low + 4 > high) {
    FontSetSelectSort(p, base, low, high);
    return;
  }
  if (low + 1 == high)
    return;
  uint32_t mid = (low + high) / 2, a, b, t;
  FontSetTimSortI(p, ex, base, low, mid);
  FontSetTimSortI(p, ex, base, mid, high);
  a = low;
  b = mid;
  t = low;
  for (uint32_t i = low; i < high; i++)
    ex[i] = p[i];
  while (a < mid && b < high) {
    const wchar_t *sa = base + ex[a].face;
    const wchar_t *sb = base + ex[b].face;
    if (FontPairCmp(base, &ex[a], &ex[b]) <= 0) {
      // if (FlStrCmpIW(base + ex[a].face, base + ex[b].face) <= 0) {
      p[t++] = ex[a++];
    } else {
      p[t++] = ex[b++];
    }
  }
  while (a < mid)
    p[t++] = ex[a++];
  while (b < high)
    p[t++] = ex[b++];
}

static void FontSetTimSort(font_pair_t *p,
                           wchar_t *base,
                           uint32_t num,
                           allocator_t *alloc) {
  font_pair_t *extra =
      (font_pair_t *)alloc->alloc(NULL, num * sizeof extra[0], alloc->arg);
  if (extra == NULL) {
    FontSetSelectSort(p, base, 0, num);
  } else {
    FontSetTimSortI(p, extra, base, 0, num);
    alloc->alloc(extra, 0, alloc->arg);
  }
}

// implementation

struct _font_set_t {
  str_db_t db;
  font_pair_t *pair;
  void *map_ptr;
  HANDLE map_handle;
  font_set_stat_t stat;
  allocator_t *alloc;
};

int FontSetCreate(allocator_t *alloc, font_set_t **out) {
  font_set_t *r = alloc->alloc(NULL, sizeof *r, alloc->arg);
  *out = NULL;
  if (r != NULL) {
    StrDbCreate(alloc, &r->db);
    *out = r;
    return FL_OK;
  }
  return FL_OUT_OF_MEMORY;
}

int FontSetFree(font_set_t *set) {
  if (set == NULL)
    return 0;
  const allocator_t alloc = set->db.alloc;
  if (set->map_ptr == NULL) {
    StrDbFree(&set->db);
  } else {
    UnmapViewOfFile(set->map_ptr);
    CloseHandle(set->map_handle);
  }
  alloc.alloc(set->pair, 0, alloc.arg);
  alloc.alloc(set, 0, alloc.arg);
  return FL_OK;
}

int FontSetAdd(font_set_t *set, const wchar_t *tag, void *buffer, size_t size) {
  if (set->map_ptr != NULL)
    return FL_OS_ERROR;
  int r;
  const uint32_t pos = StrDbTell(&set->db);
  r = StrDbPushU16le(&set->db, tag, 0);
  if (r != FL_OK)
    return r;
  const uint32_t pos_fn = StrDbTell(&set->db);

  font_file_parse_t ctx = {&set->db, pos_fn, 0};
  r = FontParseTTC(buffer, (char *)buffer + size, FontNameCb2, &ctx);
  if (r == FL_UNRECOGNIZED) {
    StrDbRewind(&set->db, pos_fn);
    ctx.count = 0;
    r = FontParseOTF(buffer, (char *)buffer + size, NULL, FontNameCb2, &ctx);
  }
  if (r == FL_OK) {
    r = StrDbPushU16le(&set->db, L"", 0);
    set->stat.num_faces += ctx.count;
    set->stat.num_files++;
    // wprintf(L"  %d added\n", ctx.count);
  } else {
    StrDbRewind(&set->db, pos);
    // wprintf(L"  error\n");
  }
  return FL_OK;
}

int FontSetBuildIndex(font_set_t *set) {
  allocator_t *alloc = &set->db.alloc;
  if (set->stat.num_faces == 0) {
    // no font face to index
    alloc->alloc(set->pair, 0, alloc->arg);
    set->pair = NULL;
    return FL_OK;
  }
  // allocate map array
  font_pair_t *p = (font_pair_t *)alloc->alloc(
      set->pair, set->stat.num_faces * sizeof *p, alloc->arg);
  if (p == NULL)
    return FL_OUT_OF_MEMORY;
  set->pair = p;

  // reset counter
  uint32_t expect_faces = set->stat.num_faces;
  set->stat.num_files = 0;
  set->stat.num_faces = 0;

  // iter through data
  wchar_t *buf = set->db.buffer;
  uint32_t pos = 0;
  uint32_t i = 0;
  while (pos < set->db.pos && set->stat.num_faces < expect_faces) {
    const uint32_t pos_f = pos;
    uint32_t pos_v = (uint32_t)-1;
    const wchar_t *str_filename = &buf[pos];
    // wprintf(L"%s\n", &buf[pos]);
    set->stat.num_files++;

    pos = StrDbNext(&set->db, pos);
    while (pos < set->db.pos && buf[pos] != 0 &&
           set->stat.num_faces < expect_faces) {
      const wchar_t *str_line = &buf[pos];
      // wprintf(L"  %d.%s\n", tc, &buf[pos]);
      if (buf[pos] == kTagVersion[0] && buf[pos + 1] == kTagVersion[1] &&
          buf[pos + 2] == kTagVersion[2]) {
        pos_v = pos + 3;
      } else {
        set->stat.num_faces++;
        p[i].face = pos;
        p[i].tag = pos_f;
        p[i].ver = pos_v;
        i++;
      }
      pos = StrDbNext(&set->db, pos);
    }
    pos = StrDbNext(&set->db, pos);
    // wprintf(L"  %d added\n", tc);
  }
  if (set->stat.num_faces != expect_faces)
    return FL_CORRUPTED;

  // FontSetQSort(p, buf, 0, i - 1);
  FontSetTimSort(p, buf, i, alloc);

  // debug
  font_set_t *r = set;
  if (0) {
    HANDLE fp = CreateFile(L"index_face.txt", GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    for (uint32_t i = 0; i < r->stat.num_faces; i++) {
      const wchar_t *font = StrDbGet(&r->db, r->pair[i].face);
      DWORD cch = 2 * FlStrLenW(font);
      WriteFile(fp, font, cch, &cch, NULL);
      wchar_t eol[2] = L"\n";
      WriteFile(fp, eol, 2, &cch, NULL);
    }
    CloseHandle(fp);
  }
  for (uint32_t i = 0; i < r->stat.num_faces * 0; i++) {
    const wchar_t *font = StrDbGet(&r->db, r->pair[i].face);
    if (r->pair[i].ver == (uint32_t)-1) {
      int stop = 0;
    } else {
      const wchar_t *ver = StrDbGet(&r->db, r->pair[i].ver) + 3;
      int stop = 0;
    }
    int stop = 0;
    //   wprintf(L"%s\n", StrDbGet(&r->db, r->pair[i].face));
  }
  return FL_OK;
}

const wchar_t *FontSetLookup(font_set_t *set, const wchar_t *face) {
  if (set->pair == NULL || set->stat.num_faces == 0)
    return NULL;
  int a = 0;
  int b = set->stat.num_faces - 1;
  if (face[0] == L'@')
    face++;
  while (a <= b) {
    int m = (a + b) / 2;
    const wchar_t *got = StrDbGet(&set->db, set->pair[m].face);
    int t = FlStrCmpIW(face, got);
    if (t == 0) {
      // find the latest
      while (m + 1 != set->stat.num_faces &&
             FlStrCmpIW(face, StrDbGet(&set->db, set->pair[m + 1].face)) == 0) {
        m++;
      }
      return StrDbGet(&set->db, set->pair[m].tag);
    } else if (t > 0) {
      a = m + 1;
    } else {
      b = m - 1;
    }
  }
  return NULL;
}

void FontSetStat(font_set_t *set, font_set_stat_t *stat) {
  *stat = set->stat;
}

int FontSetLookupIter(font_set_t *set, const wchar_t *face, font_iter_t *iter) {
  if (set->pair == NULL || set->stat.num_faces == 0 || iter == NULL)
    return 0;

  uint32_t a = 0;
  uint32_t b = set->stat.num_faces - 1;
  uint32_t m;
  while (a <= b) {
    m = a + (b - a) / 2;
    const wchar_t *got = StrDbGet(&set->db, set->pair[m].face);
    int t = FlStrCmpIW(face, got);
    if (t == 0) {
      // find the first
      while (m > 0 &&
             FlStrCmpIW(face, StrDbGet(&set->db, set->pair[m - 1].face)) == 0)
        m--;
      // find the latest
      const wchar_t *latest_ver = StrDbGet(&set->db, set->pair[m].ver);
      for (uint32_t x = m + 1; x != set->stat.num_faces; x++) {
        const wchar_t *got_face = StrDbGet(&set->db, set->pair[x].face);
        const wchar_t *got_ver = StrDbGet(&set->db, set->pair[x].ver);
        if (FlStrCmpIW(face, got_face) != 0)
          break;
        if (latest_ver == NULL || got_ver == NULL ||
            FlVersionCmp(latest_ver, got_ver) < 0) {
          m = x;
          latest_ver = got_ver;
        }
      }
      a = b = m;
      break;
    } else if (t > 0) {
      a = m + 1;
    } else {
      b = m - 1;
    }
  }

  if (a != b || a != m) {
    // not found
    iter->query_id = 0;
    iter->file_id = 0;
    iter->filename = NULL;
    iter->version = NULL;
    return 0;
  }

  iter->set = set;
  iter->query_id = m;
  iter->set_id = m;
  iter->file_id = set->pair[m].tag;
  iter->filename = StrDbGet(&set->db, set->pair[m].tag);
  iter->version = StrDbGet(&set->db, set->pair[m].ver);
  return 1;
}

int FontSetLookupIterNext(font_iter_t *iter) {
  font_set_t *set = iter->set;
  const wchar_t *match_face =
      StrDbGet(&set->db, set->pair[iter->query_id].face);
  const wchar_t *match_ver = StrDbGet(&set->db, set->pair[iter->query_id].ver);
  size_t len = FlStrLenW(match_face);

  while ((++iter->set_id) != set->stat.num_faces) {
    const wchar_t *got_face = StrDbGet(&set->db, set->pair[iter->set_id].face);
    const wchar_t *got_ver = StrDbGet(&set->db, set->pair[iter->set_id].ver);
    if (FlStrCmpNW(match_face, got_face, len) != 0) {
      break;
    } else if (FlStrCmpW(match_ver, got_ver) == 0) {
      // yield
      iter->file_id = set->pair[iter->set_id].tag;
      iter->filename = StrDbGet(&set->db, set->pair[iter->set_id].tag);
      iter->version = StrDbGet(&set->db, set->pair[iter->set_id].ver);
      return 1;
    }
  }
  // end
  iter->query_id = 0;
  iter->set_id = 0;
  iter->file_id = 0;
  iter->filename = NULL;
  iter->version = NULL;
  return 0;
}

const wchar_t *FontSetLookupFileId(font_set_t *set, uint32_t id) {
  return StrDbGet(&set->db, id);
}

// Windows related

int FontSetLoad(const wchar_t *path, allocator_t *alloc, font_set_t **out) {
  int succ = 0;
  HANDLE h = INVALID_HANDLE_VALUE, hm = INVALID_HANDLE_VALUE;
  char *ptr = NULL;
  font_set_t *r = NULL;
  do {
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;
    hm = CreateFileMapping(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (hm == INVALID_HANDLE_VALUE)
      break;
    ptr = MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
    if (ptr == NULL)
      break;
    const size_t size = GetFileSize(h, NULL);

    const uint32_t *head = (uint32_t *)ptr;
    if (head[0] != FONT_DB_TAG_MAGIC)
      break;
    // check file size
    if (size != head[3])
      break;
    // check NUL exists
    if (head[3] < 8)
      break;
    const wchar_t *buf_tail = (wchar_t *)(ptr + head[3]);
    if (buf_tail[-1] != 0 && buf_tail[-2] != 0)
      break;

    if (FontSetCreate(alloc, &r) != FL_OK)
      break;

    r->db.buffer = (wchar_t *)(head + 4);
    r->db.pos = (head[3] - 16) / 2;
    r->stat.num_faces = head[2];
    if (FontSetBuildIndex(r) != FL_OK)
      break;

    // all green
    r->map_ptr = ptr;
    r->map_handle = hm;
    // r->pair = (font_pair_t *)(ptr + head[3]);
    *out = r;
    succ = 1;
    // wprintf(L"from cache %d face, %d file\n", r->stat.num_faces,
    // r->stat.num_files);
  } while (0);

  CloseHandle(h);
  if (!succ) {
    if (ptr)
      UnmapViewOfFile(ptr);
    CloseHandle(hm);
  }
  return succ ? FL_OK : FL_OS_ERROR;
}

int FontSetDump(font_set_t *set, const wchar_t *path) {
  int succ = 0, ret = FL_OK;
  HANDLE h = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  do {
    if (h == INVALID_HANDLE_VALUE)
      break;
    uint32_t head[4] = {FONT_DB_TAG_MAGIC, set->stat.num_files,
                        set->stat.num_faces};
    head[3] = sizeof head + set->db.pos * sizeof set->db.buffer[0];

    DWORD dw_out;
    if (!WriteFile(h, head, sizeof head, &dw_out, NULL))
      break;
    if (!WriteFile(h, set->db.buffer, set->db.pos * sizeof set->db.buffer[0],
                   &dw_out, NULL))
      break;
    succ = 1;
  } while (0);
  CloseHandle(h);
  return succ ? FL_OK : FL_OS_ERROR;
}
