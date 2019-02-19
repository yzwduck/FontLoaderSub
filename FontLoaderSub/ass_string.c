#include "ass_string.h"

int ass_is_space(int ch) {
  return ch == ' ' || ch == '\t';
}

void ass_trim(ASS_Range *r) {
  if (!r || !r->begin || !r->end || r->begin == r->end)
    return;
  for (; r->begin != r->end && ass_is_space(*r->begin); r->begin++) {
    // nop;
  }
  if (r->begin == r->end)
    return;
  for (; ass_is_space(r->end[-1]); r->end--) {
    // nop;
  }
}

const wchar_t *ass_skip_spaces(const wchar_t *p, const wchar_t *end) {
  for (; ass_is_space(*p) && p != end; p++) {
    // nop
  }
  return p;
}

int ass_is_eol(int ch) {
  return ch == '\r' || ch == '\n';
}

int ass_strncmp(const wchar_t *s1, const wchar_t *s2, size_t cch) {
  wchar_t a, b;
  const wchar_t *last = s2 + cch;

  do {
    a = *s1++;
    b = *s2++;
  } while (s2 != last && a && a == b);

  return a - b;
}

static wchar_t ass_to_lower(wchar_t ch) {
  if (L'A' <= ch && ch <= L'Z')
    return ch - L'A' + L'a';
  return ch;
}

int ass_strncasecmp(const wchar_t *s1, const wchar_t *s2, size_t cch) {
  // assume wchar_t is unsigned short
  wchar_t a, b;
  const wchar_t *last = s2 + cch;

  do {
    a = ass_to_lower(*s1++);
    b = *s2++;
  } while (s2 != last && a && a == b);

  return a - b;
}

const wchar_t *ass_strnchr(const wchar_t *s, wchar_t ch, size_t cch) {
  const wchar_t *last = s + cch;

  for (; s != last && *s != ch; s++) {
    // nop
  }
  return s == last ? NULL : s;
}

size_t ass_strlen(const wchar_t *str) {
  const wchar_t *p;
  for (p = str; *p; p++) {
    // nop
  }
  return p - str;
}

size_t ass_strnlen(const wchar_t *str, size_t n) {
  for (size_t i = 0; i != n; i++) {
    if (str[i] == 0)
      return i;
  }
  return n;
}
