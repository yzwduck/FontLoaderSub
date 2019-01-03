#include "ssa_parser.h"

typedef struct {
  const wchar_t *str;
  const wchar_t *eos;
} StringRange;

static int is_space(const wchar_t ch) {
  switch (ch) {
  case L' ':
  case L'\t':
    return 1;
  default:
    return 0;
  }
}

void string_range_trim(StringRange *s) {
  if (s == NULL || s->str == NULL || s->eos == NULL || s->str == s->eos)
    return;
  for (; s->str != s->eos && is_space(*s->str); s->str++) {
    // nop
  }
  if (s->str == s->eos)
    return;
  for (; is_space(s->eos[-1]); s->eos--) {
    // nop
  }
}

const wchar_t *str_next_crlf(const wchar_t *s) {
  for (; *s; s++) {
    if (*s == L'\r' || *s == L'\n')
      return s;
  }
  return NULL;
}

static int TryCode(UINT codepage,
                   const char *mstr,
                   int bytes,
                   StringRange *o,
                   allocator_t *alloc) {
  int r = MultiByteToWideChar(codepage, 0, mstr, bytes, NULL, 0);
  if (r != 0) {
    wchar_t *buf =
        (wchar_t *)alloc->alloc(NULL, r * sizeof(wchar_t) + 4, alloc->arg);
    if (buf == NULL)
      return 0;
    o->str = buf;
    o->eos = buf + r;
    r = MultiByteToWideChar(codepage, 0, mstr, bytes, buf, r);
    if (r == 0) {
      o->str = o->eos = NULL;
      HeapFree(GetProcessHeap(), 0, buf);
    } else {
      string_range_trim(o);
    }
  }
  return r;
}

static int TryU16(int big_endian,
                  const char *str,
                  int bytes,
                  StringRange *o,
                  allocator_t *alloc) {
  if (bytes % 2 != 0)
    return 0;
  wchar_t *buf = (wchar_t *)alloc->alloc(NULL, bytes + 4, alloc->arg);
  if (buf == NULL)
    return 0;
  int cch = bytes / 2;
  FlStrCpyNW(buf, (const wchar_t *)str, cch);
  if (big_endian) {
    for (int i = 0; i < cch; i++) {
      buf[i] = be16(buf[i]);
    }
  }
  o->str = buf;
  o->eos = buf + cch;
  string_range_trim(o);
  return cch;
}

static int csv_is_space(const wchar_t ch) {
  switch (ch) {
  case L' ':
  case L'\t':
    return 1;
  default:
    return 0;
  }
}

static int csv_is_split(const wchar_t ch) {
  return ch == L',';
}

static const wchar_t *csv_next_tok1(StringRange *input, StringRange *tok) {
  const wchar_t *ptr = input->str;
  const wchar_t *eos = input->eos;

  if (ptr == NULL || ptr == eos)
    return NULL;
  tok->str = NULL;
  tok->eos = NULL;

  // step 1: skip heading spaces
  for (; ptr != eos; ptr++) {
    if (!csv_is_space(*ptr))
      break;
  }
  if (ptr == eos)
    return NULL;

  // step 2: receive buffer
  tok->str = ptr;
  for (; ptr != eos && !csv_is_split(*ptr); ptr++) {
    if (!csv_is_space(*ptr))
      tok->eos = ptr + 1;
  }

  // step 3: skip for next split (if exist)
  if (ptr != eos && csv_is_split(*ptr))
    ptr++;

  return ptr;
}

static int csv_next_tok(StringRange *input, StringRange *tok) {
  const wchar_t *ptr = input->str;
  const wchar_t *eos = input->eos;

  if (ptr == NULL || eos == NULL || ptr == eos) {
    input->str = NULL;
    return -1;
  }
  tok->str = ptr;
  tok->eos = NULL;

  for (; ptr != eos && !csv_is_split(*ptr); ptr++) {
    tok->eos = ptr + 1;
  }

  // if length==0, reset
  if (tok->eos == NULL) {
    tok->eos = ptr;
  }

  string_range_trim(tok);
  // skip next split
  if (ptr != eos && csv_is_split(*ptr))
    ptr++;

  input->str = ptr;
  return 0;
}

static int ass_next_tags(StringRange *input, StringRange *tags, int *is_tag) {
  const wchar_t *ptr = input->str;
  const wchar_t *eos = input->eos;
  const wchar_t *eot = NULL;
  *is_tag = 0;

  if (ptr == NULL || eos == NULL || ptr == eos) {
    input->str = NULL;
    return -1;
  }
  tags->str = ptr;
  tags->eos = NULL;

  if (*ptr == L'{') {
    eot = FlStrChrNW(ptr, L'}', eos - ptr);
    if (eot != NULL) {
      // found matching tag
      tags->str = ptr + 1;
      tags->eos = eot;
      input->str = eot + 1;
      *is_tag = 1;
    } else {
      // found left brace only
      tags->str = ptr;
      tags->eos = eos;
      input->str = eos;
      *is_tag = 0;
    }
  } else {
    // not starts with a tag
    eot = FlStrChrNW(ptr, L'{', eos - ptr);
    if (eot == NULL)
      eot = eos;
    tags->str = ptr;
    tags->eos = eot;
    input->str = eot;
    *is_tag = 0;
  }

  string_range_trim(tags);
  return 0;
}

static int ass_next_tag(StringRange *tags, StringRange *tag) {
  const wchar_t *ptr = tags->str;
  const wchar_t *eos = tags->eos;
  const wchar_t *eot = NULL;

  if (ptr == NULL || eos == NULL || ptr == eos) {
    tags->str = NULL;
    return -1;
  }
  // if (ptr[0] != L'\\') return -1;

  eot = FlStrChrNW(ptr + 1, L'\\', eos - ptr - 1);
  if (eot == NULL)
    eot = eos;
  tag->str = ptr;
  tag->eos = eot;
  string_range_trim(tag);
  tags->str = eot;
  return 0;
}

enum ass_ext_state_t { Idle, GotStyles, GotEvents };

static int ReportFontFamily(str_db_t *fonts, StringRange *tok) {
  const uint32_t pos = StrDbTell(fonts);
  if (StrDbPushU16le(fonts, tok->str, tok->eos - tok->str) == 0) {
    if (StrDbIsDuplicate(fonts, 0, pos)) {
      StrDbRewind(fonts, pos);
    }
  }
  return 0;
}

static int ass_style(StringRange *l, ssa_parse_font_cb_t cb, void *arg) {
  int r = 0;
  if (FlStrCmpNW(l->str, L"Style:", min(l->eos - l->str, 6)) == 0) {
    l->str += 6;
  } else {
    return 0;
  }
  string_range_trim(l);
  StringRange tok[1];

  csv_next_tok(l, tok);      // name
  r = csv_next_tok(l, tok);  // FontName
  if (0) {
    csv_next_tok(l, tok);  // FontSize
    csv_next_tok(l, tok);  // Primary
    csv_next_tok(l, tok);  // Secondary
    csv_next_tok(l, tok);  // Outline/Teriary
    csv_next_tok(l, tok);  // Shadow/Outline

    csv_next_tok(l, tok);  // bold
    csv_next_tok(l, tok);  // italic
  }
  if (r == 0) {
    cb(tok->str, tok->eos - tok->str, arg);
    // ReportFontFamily(fonts, tok);
  }
  return 0;
}

static int ass_event(StringRange *l, ssa_parse_font_cb_t cb, void *arg) {
  if (FlStrCmpNW(l->str, L"Dialogue:", min(l->eos - l->str, 9)) == 0) {
    l->str += 9;
  } else if (FlStrCmpNW(l->str, L"Comment:", min(l->eos - l->str, 8)) == 0) {
    l->str += 8;
  } else {
    return 0;
  }
  string_range_trim(l);
  StringRange tok[1], tags[1], tag[1];

  csv_next_tok(l, tok);  // Layer
  csv_next_tok(l, tok);  // Start
  csv_next_tok(l, tok);  // End
  csv_next_tok(l, tok);  // Style
  csv_next_tok(l, tok);  // Name
  csv_next_tok(l, tok);  // MarginL
  csv_next_tok(l, tok);  // MarginR
  csv_next_tok(l, tok);  // MarginV
  csv_next_tok(l, tok);  // Effect
  tok->str = l->str;
  tok->eos = l->eos;

  while (tok->str) {
    int is_tag;
    ass_next_tags(tok, tags, &is_tag);
    if (is_tag) {
      while (tags->str) {
        if (ass_next_tag(tags, tag) == 0) {
          if (tag->eos - tag->str > 3 &&
              FlStrCmpNW(tag->str, L"\\fn", 3) == 0) {
            tag->str += 3;
            cb(tag->str, tag->eos - tag->str, arg);
          }
        }
      }
    }
  }
  return 0;
}

int AssParseFont(const wchar_t *str,
                 size_t cch,
                 ssa_parse_font_cb_t cb,
                 void *arg) {
  StringRange data[1];
  if (cch == 0)
    cch = FlStrLenW(str);
  data->str = str;
  data->eos = str + cch;
  const wchar_t *ptr = data->str;
  enum ass_ext_state_t state = Idle;
  while (ptr != data->eos) {
    const wchar_t *eol = str_next_crlf(ptr);
    if (eol == NULL) {
      // EOF
      break;
    }
    if (ptr != eol && ptr[0] != L';') {
      // non-empty line
      if (ptr[0] == L'[' && eol[-1] == L']') {
        // header
        state = Idle;
        if (FlStrCmpNIW(ptr, L"[v4 styles]", eol - ptr) == 0 ||
            FlStrCmpNIW(ptr, L"[v4+ styles]", eol - ptr) == 0) {
          state = GotStyles;
        } else if (FlStrCmpNIW(ptr, L"[Events]", eol - ptr) == 0) {
          state = GotEvents;
        }
      } else {
        StringRange l;
        l.str = ptr;
        l.eos = eol;
        if (state == GotStyles)
          ass_style(&l, cb, arg);
        else if (state == GotEvents)
          ass_event(&l, cb, arg);
      }
    }
    ptr = eol + 1;
  }
  return 0;
}

wchar_t *TextFileFromPath(const wchar_t *path,
                          size_t *cch,
                          allocator_t *alloc) {
  int succ = 0;
  HANDLE h = INVALID_HANDLE_VALUE, hm = INVALID_HANDLE_VALUE;
  char *buf = NULL;
  StringRange o[1] = {NULL};
  memmap_t mmap;

  do {
    FlMemMap(path, &mmap);
    if (mmap.data == NULL)
      break;
    buf = (char *)mmap.data;
    const size_t size = mmap.size;
    if (size < 4)
      break;

    if (buf[0] == '\xef' && buf[1] == '\xbb' && buf[2] == '\xbf') {
      // UTF-8
      TryCode(CP_UTF8, buf + 3, size - 3, o, alloc);
    } else if (buf[0] == '\xff' && buf[1] == '\xfe') {
      TryU16(0, buf + 2, size - 2, o, alloc);
    } else if (buf[0] == '\xfe' && buf[1] == '\xff') {
      TryU16(1, buf + 2, size - 2, o, alloc);
    } else {
      if (TryCode(CP_UTF8, buf, size, o, alloc) == 0) {
        TryCode(CP_ACP, buf, size, o, alloc);
      }
    }
  } while (0);
  FlMemUnmap(&mmap);
  if (cch)
    *cch = o->eos - o->str;
  return (wchar_t *)o->str;
}
