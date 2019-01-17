#pragma once

#include "util.h"

typedef struct _vec_t {
  void *data;
  size_t n;
  size_t capacity;
  size_t size;
  allocator_t *alloc;
} vec_t;

int vec_init(vec_t *v, size_t size, allocator_t *alloc);

int vec_free(vec_t *v);

size_t vec_prealloc(vec_t *v, size_t n);

int vec_append(vec_t *v, void *data, size_t n);

typedef struct _str_buf_t {
  vec_t vec;
  wchar_t ex_pad;
  uint16_t pad_len;
} str_db_t;

int str_db_init(
    str_db_t *s,
    allocator_t *alloc,
    wchar_t ex_pad,
    uint16_t pad_len);

int str_db_free(str_db_t *s);

size_t str_db_tell(str_db_t *s);

size_t str_db_seek(str_db_t *s, size_t pos);

const wchar_t *str_db_next(str_db_t *s, size_t *next_pos);

const wchar_t *str_db_get(str_db_t *s, size_t pos);

const wchar_t *str_db_push_prefix(str_db_t *s, const wchar_t *str, size_t cch);

const wchar_t *str_db_push_u16_le(str_db_t *s, const wchar_t *str, size_t cch);

const wchar_t *str_db_push_u16_be(str_db_t *s, const wchar_t *str, size_t cch);

const wchar_t *str_db_str(str_db_t *s, size_t pos, const wchar_t *str);

void str_db_loads(str_db_t *s, const wchar_t *str, size_t cch, wchar_t ex_pad);
