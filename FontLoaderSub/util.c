#include <Windows.h>
#include "util.h"

int FlMemMap(const wchar_t *path, memmap_t *mmap) {
  mmap->map = NULL;
  mmap->data = NULL;
  mmap->size = 0;
  HANDLE h;
  do {
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                   FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;
    mmap->map = CreateFileMapping(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (mmap->map == INVALID_HANDLE_VALUE)
      break;
    mmap->data = MapViewOfFile(mmap->map, FILE_MAP_READ, 0, 0, 0);
    if (mmap->data == NULL)
      break;
    DWORD high = 0;
    mmap->size = GetFileSize(h, &high);
    // mmap->size = (high * 0x100000000UL) | (mmap->size);
  } while (0);

  if (mmap->data == NULL) {
    FlMemUnmap(mmap);
  }
  CloseHandle(h);
  return 0;
}

int FlMemUnmap(memmap_t *mmap) {
  UnmapViewOfFile(mmap->data);
  CloseHandle(mmap->map);
  mmap->map = NULL;
  mmap->data = NULL;
  mmap->size = 0;
  return 0;
}

static int FlTestUtf8(const uint8_t *buffer, size_t size) {
  const uint8_t *p, *last;
  int rem = 0;
  for (p = buffer, last = buffer + size; p != last; p++) {
    if (rem) {
      if ((*p & 0xc0) == 0x80) {
        // 10xxxxxx
        --rem;
      } else {
        return 0;
      }
    } else if ((*p & 0x80) == 0) {
      // rem = 0;
    } else if ((*p & 0xd0) == 0xc0) {
      // 110xxxxx
      rem = 1;
    } else if ((*p & 0xf0) == 0xe0) {
      // 1110xxxx
      rem = 2;
    } else if ((*p & 0xf8) == 0xf0) {
      // 11110xxx
      rem = 3;
    } else {
      return 0;
    }
  }
  return rem == 0;
}

static wchar_t *FlTextTryDecode(
    UINT codepage,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *buf = NULL;
  int ok = 0;
  do {
    const int r =
        MultiByteToWideChar(codepage, 0, (const char *)mstr, bytes, NULL, 0);
    *cch = r;
    if (r == 0)
      break;

    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == NULL)
      break;

    const int new_r =
        MultiByteToWideChar(codepage, 0, (const char *)mstr, bytes, buf, r);
    if (new_r == 0 || new_r != r)
      break;
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = NULL;
  }
  return buf;
}

static wchar_t *FlTextDecodeUtf16(
    int big_endian,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *buf = NULL;
  int ok = 0;

  do {
    const size_t r = *cch = bytes / 2;
    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == NULL)
      break;

    for (size_t i = 0; i != r; i++) {
      buf[i] = big_endian ? be16(mstr[i]) : mstr[i];
    }
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = NULL;
  }
  return buf;
}

wchar_t *FlTextDecode(
    const uint8_t *buf,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *res = NULL;
  if (bytes < 4)
    return res;

  // detect BOM
  if (buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
    res = FlTextTryDecode(CP_UTF8, buf + 3, bytes - 3, cch, alloc);
  } else if (buf[0] == 0xff && buf[1] == 0xfe) {
    // UTF-16 LE
    res = FlTextDecodeUtf16(0, buf + 2, bytes - 2, cch, alloc);
  } else if (buf[0] == 0xfe && buf[1] == 0xff) {
    // UTF-16 BE
    res = FlTextDecodeUtf16(1, buf + 2, bytes - 2, cch, alloc);
  }

  // detect UTF-8
  if (!res && FlTestUtf8(buf, bytes)) {
    res = FlTextTryDecode(CP_UTF8, buf, bytes, cch, alloc);
  }
  // final resort
  if (!res) {
    res = FlTextTryDecode(CP_ACP, buf, bytes, cch, alloc);
  }
  return res;
}
