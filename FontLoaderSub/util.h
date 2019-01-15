#pragma once

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

typedef struct {
  HANDLE map;
  void *data;
  size_t size;
} memmap_t;

int FlMemMap(const wchar_t *path, memmap_t *mmap);

int FlMemUnmap(memmap_t *mmap);

wchar_t *
FlTextDecode(const uint8_t *buf, size_t bytes, size_t *cch, allocator_t *alloc);
