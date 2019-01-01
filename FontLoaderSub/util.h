#ifndef FL_UTIL_H
#define FL_UTIL_H

#include <Windows.h>
#include <stdint.h>

inline uint16_t be16(uint16_t v) {
  // check if compiles to `XCHG`
  // alter: _byteswap_ushort
  return (v >> 8) | (v << 8);
}

inline uint32_t be32(uint32_t num) {
  // check if compiles to `BSWAP`
  // alter: _byteswap_ulong
  return ((num >> 24) & 0xff) |       // move byte 3 to byte 0
         ((num << 8) & 0xff0000) |    // move byte 1 to byte 2
         ((num >> 8) & 0xff00) |      // move byte 2 to byte 1
         ((num << 24) & 0xff000000);  // byte 0 to byte 3
}

typedef struct _allocator_t {
  void *(*alloc)(void *existing, size_t size, void *arg);
  void *arg;
} allocator_t;

typedef int (*file_walk_cb_t)(const wchar_t *full_path,
                              WIN32_FIND_DATA *data,
                              void *arg);

int WalkDir(const wchar_t *path,
            file_walk_cb_t callback,
            void *arg,
            allocator_t *alloc);

// String DB

struct _str_db_t {
  wchar_t *buffer;
  size_t size;
  size_t pos;
  allocator_t alloc;
  wchar_t ex_pad;
  uint16_t pad_len;
};

typedef struct _str_db_t str_db_t;

int StrDbCreate(allocator_t *alloc, str_db_t *sb);

int StrDbFree(str_db_t *sb);

size_t StrDbTell(str_db_t *sb);

size_t StrDbNext(str_db_t *sb, size_t pos);

int StrDbRewind(str_db_t *sb, size_t pos);

int StrDbPreAlloc(str_db_t *sb, size_t cch);

const wchar_t *StrDbGet(str_db_t *sb, size_t pos);

int StrDbPushU16le(str_db_t *sb, const wchar_t *str, size_t cch);

int StrDbPushPrefix(str_db_t *sb, const wchar_t *str, size_t cch);

int StrDbPushU16be(str_db_t *sb, const wchar_t *str, size_t cch);

int StrDbIsDuplicate(str_db_t *sb, size_t start, size_t target);

int StrDbFullPath(str_db_t *sb, HANDLE handle);

// plain string utils

wchar_t *FlStrCpyW(wchar_t *dst, const wchar_t *src);

wchar_t *FlStrCpyNW(wchar_t *dst, const wchar_t *src, size_t cch);

size_t FlStrLenW(const wchar_t *str);

int FlStrCmpW(const wchar_t *a, const wchar_t *b);

int FlStrCmpIW(const wchar_t *a, const wchar_t *b);

int FlStrCmpNW(const wchar_t *a, const wchar_t *b, size_t len);

int FlStrCmpNIW(const wchar_t *a, const wchar_t *b, size_t len);

wchar_t *FlStrChrNW(const wchar_t *s, wchar_t ch, size_t len);

// return code

enum FL_STATUS {
  FL_OK = 0,
  FL_OS_ERROR = 1,
  FL_OUT_OF_MEMORY = 2,
  FL_UNRECOGNIZED = 3,
  FL_CORRUPTED = 4,
};

// compare two version string
int FlVersionCmp(const wchar_t *a, const wchar_t *b);

#endif
