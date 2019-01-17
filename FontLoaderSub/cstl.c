#include "cstl.h"
#include "ass_string.h"

int vec_init(vec_t *v, size_t size, allocator_t *alloc) {
  *v = (vec_t){.size = size, .alloc = alloc};
  return 0;
}

int vec_free(vec_t *v) {
  if (v->alloc) {
    allocator_t *alloc = v->alloc;
    alloc->alloc(v->data, 0, alloc->arg);
  }
  return 0;
}

size_t vec_prealloc(vec_t *v, size_t n) {
  if (v->alloc && v->n + n > v->capacity) {
    allocator_t *alloc = v->alloc;
    size_t new_cap = v->capacity * 2;
    if (new_cap < n + v->n)
      new_cap = n * 2 + v->capacity * 2;
    void *new_buf = alloc->alloc(v->data, new_cap * v->size, alloc->arg);
    if (new_buf) {
      v->capacity = new_cap;
      v->data = new_buf;
    }
  }
  if (v->capacity < v->n) {
    // already corrupted, stop here
    FlBreak();
  }
  return v->capacity - v->n;
}

int vec_append(vec_t *v, void *data, size_t n) {
  const size_t space = vec_prealloc(v, n);
  if (space < n)
    return 0;

  const uint8_t *src = data;
  const uint8_t *last = &src[n * v->size];
  uint8_t *dst = v->data;
  dst += v->n * v->size;

  v->n += n;
  while (src != last) {
    *dst++ = *src++;
  }
  return 1;
}

int vec_clear(vec_t *v) {
  v->n = 0;
  return 0;
}

int str_db_init(
    str_db_t *s,
    allocator_t *alloc,
    wchar_t ex_pad,
    uint16_t pad_len) {
  const int r = vec_init(&s->vec, sizeof(wchar_t), alloc);
  s->ex_pad = ex_pad;
  s->pad_len = pad_len;
  return r;
}

int str_db_free(str_db_t *s) {
  return vec_free(&s->vec);
}

size_t str_db_tell(str_db_t *s) {
  return s->vec.n;
}

size_t str_db_seek(str_db_t *s, size_t pos) {
  if (pos <= s->vec.capacity)
    s->vec.n = pos;
#if 0
  if (pos < s->vec.capacity) {
    wchar_t *buf = (wchar_t *)s->vec.data;
    buf[pos] = 0;
  }
#endif
  return s->vec.n;
}

const wchar_t *str_db_next(str_db_t *s, size_t *next_pos) {
  if (*next_pos == s->vec.n)
    return NULL;
  const wchar_t *ret = str_db_get(s, *next_pos);
  if (ret != NULL) {
    const size_t len = ass_strlen(ret);
    *next_pos += len + s->pad_len;
  }
  return ret;
}

const wchar_t *str_db_get(str_db_t *s, size_t pos) {
  if (pos > s->vec.n)
    return NULL;
  wchar_t *buf = (wchar_t *)s->vec.data;
  return &buf[pos];
}

const wchar_t *str_db_push_prefix(str_db_t *s, const wchar_t *str, size_t cch) {
  const wchar_t *ret = str_db_push_u16_le(s, str, cch);
  if (ret) {
    s->vec.n -= s->pad_len;
  }
  return ret;
}

const wchar_t *str_db_push_u16_le(str_db_t *s, const wchar_t *str, size_t cch) {
  const size_t len = cch ? ass_strnlen(str, cch) : ass_strlen(str);
  const size_t len_all = len + s->pad_len;  // always NUL terminated
  if (vec_prealloc(&s->vec, len_all + 1) < len_all + 1)
    return NULL;

  wchar_t *ret = (wchar_t *)str_db_get(s, str_db_tell(s));
  for (size_t i = 0; i != len; i++)
    ret[i] = str[i];

  for (uint16_t i = 0; i != s->pad_len; i++)
    ret[len + i] = s->ex_pad;
  ret[len] = 0;

  s->vec.n += len_all;
  return ret;
}

const wchar_t *str_db_push_u16_be(str_db_t *s, const wchar_t *str, size_t cch) {
  wchar_t *ret = (wchar_t *)str_db_push_u16_le(s, str, cch);
  if (ret) {
    for (wchar_t *p = ret; *p; p++) {
      *p = be16(*p);
    }
  }
  return ret;
}

const wchar_t *str_db_str(str_db_t *s, size_t pos, const wchar_t *str) {
  const size_t len = ass_strlen(str) + 1;
  size_t it = pos;
  const wchar_t *sub;
  while ((sub = str_db_next(s, &it)) != NULL) {
    if (ass_strncmp(sub, str, len) == 0) {
      return sub;
    }
  }
  return NULL;
}

void str_db_loads(str_db_t *s, const wchar_t *str, size_t cch, wchar_t ex_pad) {
  // clang-format off
  *s = (str_db_t){
    .vec= (vec_t){
      .data = (void*)str,
      .n = cch,
      .capacity = cch,
      .size = sizeof str[0],
      .alloc = NULL
    },
    .ex_pad = ex_pad,
    .pad_len = ex_pad ? 2 : 1
  };
  // clang-format on
}
