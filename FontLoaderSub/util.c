#include "util.h"

#include <Shlwapi.h>

typedef struct {
  allocator_t alloc;
  file_walk_cb_t callback;
  void *arg;        // for callback
  wchar_t *buffer;  // full path
  uint32_t size;
  uint32_t pos;
} file_walk_t;

static int WalkDirDfs(file_walk_t *ctx) {
  int r = FL_OK;
  // ensure enough memory to hold the path
  // scenario 1: "abc\*" -> "abc\new\*"
  // scenario 2: "root\this" -> "root\this\*"
  if (ctx->pos + MAX_PATH + 1 > ctx->size) {
    const uint32_t new_size = ctx->pos + MAX_PATH * 2;
    void *new_buffer = ctx->alloc.alloc(
        ctx->buffer, new_size * sizeof ctx->buffer[0], ctx->alloc.arg);
    if (new_buffer == NULL)
      return FL_OUT_OF_MEMORY;
    ctx->size = new_size;
    ctx->buffer = (wchar_t *)new_buffer;
  }
  // trim last folder (when expanding path)
  uint32_t pos = ctx->pos;
  while (pos > 0 && ctx->buffer[pos - 1] != L'\\')
    pos--;
  // start
  HANDLE h;
  WIN32_FIND_DATA fd;
  h = FindFirstFile(ctx->buffer, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.cFileName[0] == L'.' && fd.cFileName[1] == 0 ||
          fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' &&
              fd.cFileName[2] == 0) {
        // nop for "." and ".."
      } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        FlStrCpyW(ctx->buffer + pos, fd.cFileName);
        while (ctx->pos != ctx->size && ctx->buffer[ctx->pos] != 0)
          ctx->pos++;
        ctx->buffer[ctx->pos++] = L'\\';
        ctx->buffer[ctx->pos++] = L'*';
        ctx->buffer[ctx->pos] = 0;
        r = WalkDirDfs(ctx);
        ctx->pos = pos;
        if (r != FL_OK)
          break;
      } else if (ctx->callback != NULL) {
        FlStrCpyW(ctx->buffer + pos, fd.cFileName);
        r = ctx->callback(ctx->buffer, &fd, ctx->arg);
        if (r != FL_OK)
          break;
      }
    } while (FindNextFile(h, &fd));
  } else {
    r = FL_OS_ERROR;
  }
  ctx->pos = pos;
  ctx->buffer[pos] = 0;
  return r;
}

int WalkDir(const wchar_t *path,
            file_walk_cb_t callback,
            void *arg,
            allocator_t *alloc) {
  int succ = 0, ret = FL_OS_ERROR;
  file_walk_t ctx = {*alloc, callback, arg, NULL, 0, 0};
  HANDLE h;
  do {
    // Open the path, either it's a file or a directory
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;
    // Query name length
    const DWORD name_flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD size = GetFinalPathNameByHandle(h, NULL, 0, name_flags);
    if (size == 0)
      break;
    ctx.pos = size;        // save returned length
    size += MAX_PATH * 2;  // pre-alloc
    // Alloc memory for full path
    wchar_t *new_buffer = (wchar_t *)alloc->alloc(
        ctx.buffer, size * sizeof ctx.buffer[0], alloc->arg);
    if (new_buffer == NULL)
      break;
    ctx.buffer = new_buffer;
    ctx.size = size;
    // Query name
    if (GetFinalPathNameByHandle(h, ctx.buffer, size, name_flags) == 0)
      break;
    // Trim NUL
    while (ctx.pos > 0 && ctx.buffer[ctx.pos - 1] == 0)
      ctx.pos--;

    BY_HANDLE_FILE_INFORMATION info;
    if (GetFileInformationByHandle(h, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
      // the `path` is a file, emulate WIN32_FIND_DATA
      WIN32_FIND_DATA emu;
      emu.dwFileAttributes = info.dwFileAttributes;
      emu.ftCreationTime = info.ftCreationTime;
      emu.ftLastAccessTime = info.ftLastAccessTime;
      emu.ftLastWriteTime = info.ftLastWriteTime;
      emu.nFileSizeHigh = info.nFileSizeHigh;
      emu.nFileSizeLow = info.nFileSizeLow;
      ret = callback(ctx.buffer, &emu, arg);
    } else {
      // normal recursive dir routine
      ctx.buffer[ctx.pos++] = L'\\';
      ctx.buffer[ctx.pos++] = L'*';
      ctx.buffer[ctx.pos] = 0;
      ret = WalkDirDfs(&ctx);
    }
  } while (0);
  CloseHandle(h);
  alloc->alloc(ctx.buffer, 0, alloc->arg);
  return ret;
}

int StrDbCreate(allocator_t *alloc, str_db_t *sb) {
  const str_db_t tmp = {*alloc};
  *sb = tmp;
  return FL_OK;
}

int StrDbFree(str_db_t *sb) {
  sb->alloc.alloc(sb->buffer, 0, sb->alloc.arg);
  return FL_OK;
}

uint32_t StrDbTell(str_db_t *sb) {
  return sb->pos;
}

uint32_t StrDbNext(str_db_t *sb, uint32_t pos) {
  while (pos < sb->pos && sb->buffer[pos] != 0) {
    pos++;
  }
  if (pos != sb->pos && sb->buffer[pos] == 0) {
    pos++;
    if (pos != sb->pos && sb->buffer[pos] == L'\n') {
      pos++;
    }
  }
  return pos;
}

int StrDbRewind(str_db_t *sb, uint32_t pos) {
  if (pos < sb->pos)
    sb->pos = pos;
  // TODO: insert NUL char before `pos`
  return FL_OK;
}

const wchar_t *StrDbGet(str_db_t *sb, uint32_t pos) {
  return sb->buffer + pos;
}

static int StrDbPreAlloc(str_db_t *sb, uint32_t cch) {
  if (sb->pos + cch > sb->size) {
    const uint32_t new_size = (sb->pos + cch) * 2;
    wchar_t *new_buf = (wchar_t *)sb->alloc.alloc(
        sb->buffer, new_size * sizeof sb->buffer[0], sb->alloc.arg);
    if (new_buf == NULL)
      return FL_OUT_OF_MEMORY;
    sb->buffer = new_buf;
    sb->size = new_size;
  }
  return FL_OK;
}

int StrDbPushU16le(str_db_t *sb, const wchar_t *str, uint32_t cch) {
  const uint32_t original_cch = cch;
  if (cch == 0)
    cch = FlStrLenW(str);
  while (cch > 0 && str[cch - 1] == 0)
    cch--;
  if (cch == 0 && original_cch != 0) {
    // WARN: not insert empty string, unless acknowledged
    return FL_OK;
  }
  int r = StrDbPreAlloc(sb, cch + 2);
  if (r != FL_OK)
    return r;
  wchar_t *buf = &sb->buffer[sb->pos];
  // FlStrCpyW(buf, str);
  for (uint32_t i = 0; i != cch; i++)
    buf[i] = str[i];
  buf[cch] = 0;
  buf[cch + 1] = L'\n';
  sb->pos += cch + 2;
  return FL_OK;
}

int StrDbPushU16be(str_db_t *sb, const wchar_t *str, uint32_t cch) {
  const uint32_t pos = sb->pos;
  int r = StrDbPushU16le(sb, str, cch);
  if (r != FL_OK)
    return r;
  wchar_t *buf = &sb->buffer[pos];
  for (uint32_t i = 0; buf[i] != 0; i++) {
    buf[i] = be16(buf[i]);
  }
  return FL_OK;
}

int StrDbIsDuplicate(str_db_t *sb, uint32_t start, uint32_t target) {
  const wchar_t *t = StrDbGet(sb, target);
  while (start < sb->pos && start < target) {
    const wchar_t *s = StrDbGet(sb, start);
    int r = FlStrCmpIW(s, t);
    if (r == 0)
      return 1;
    start = StrDbNext(sb, start);
  }
  return 0;
}

wchar_t *FlStrCpyW(wchar_t *dst, const wchar_t *src) {
  return StrCpyW(dst, src);
}

wchar_t *FlStrCpyNW(wchar_t *dst, const wchar_t *src, size_t cch) {
  return StrCpyNW(dst, src, cch);
}

uint32_t FlStrLenW(const wchar_t *str) {
  uint32_t r = 0;
  while (str[r])
    r++;
  return r;
}

int FlStrCmpW(const wchar_t *a, const wchar_t *b) {
  return StrCmpW(a, b);
}

int FlStrCmpIW(const wchar_t *a, const wchar_t *b) {
  return StrCmpIW(a, b);
}

int FlStrCmpNW(const wchar_t *a, const wchar_t *b, size_t len) {
  return StrCmpNW(a, b, len);
}

int FlStrCmpNIW(const wchar_t *a, const wchar_t *b, size_t len) {
  return StrCmpNIW(a, b, len);
}

wchar_t *FlStrChrNW(const wchar_t *s, wchar_t ch, size_t len) {
  return StrChrNW(s, ch, (UINT)len);
}
