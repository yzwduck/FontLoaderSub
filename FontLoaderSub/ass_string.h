#pragma once

#include <stddef.h>

typedef struct _ASS_Range {
  const wchar_t *begin;
  const wchar_t *end;
} ASS_Range;

void ass_trim(ASS_Range *r);

const wchar_t *ass_skip_spaces(const wchar_t *p, const wchar_t *end);

int ass_is_eol(int ch);

int ass_strncmp(const wchar_t *s1, const wchar_t *s2, size_t cch);

/**
 * \brief Compare two strings in lowercase
 * \param s1 first string
 * \param s2 second string, must in lowercase
 * \param cch number of chars
 * \return
 */
int ass_strncasecmp(const wchar_t *s1, const wchar_t *s2, size_t cch);

const wchar_t *ass_strnchr(const wchar_t *s, wchar_t ch, size_t cch);

size_t ass_strlen(const wchar_t *str);

size_t ass_strnlen(const wchar_t *str, size_t n);
