#include "util.h"

#include <Shlwapi.h>

typedef struct {
  file_walk_cb_t callback;
  void *arg;  // for callback
  str_db_t sb_path;
} file_walk_t;

static int WalkDirDfs(file_walk_t *ctx) {
  int r = FL_OK;
  str_db_t *sb_path = &ctx->sb_path;

  // ensure enough memory to hold the path
  // scenario 1: "abc\*" -> "abc\new\*"
  // scenario 2: "root\this" -> "root\this\*"
  r = StrDbPreAlloc(sb_path, MAX_PATH + 1);
  if (r != FL_OK)
    return FL_OUT_OF_MEMORY;

  WIN32_FIND_DATA fd;
  HANDLE find_handle = FindFirstFile(sb_path->buffer, &fd);

  // trim until last '\'
  if (1) {
    size_t pos = StrDbTell(sb_path);
    const wchar_t *buf = StrDbGet(sb_path, 0);
    while (pos > 0 && buf[pos - 1] != L'\\')
      pos--;
    StrDbRewind(sb_path, pos);
  }
  const size_t root_pos = StrDbTell(sb_path);

  do {
    if (find_handle == INVALID_HANDLE_VALUE) {
      // r = FL_OS_ERROR;
      break;
    }

    fd.cFileName[MAX_PATH - 1] = 0;  // fail-safe
    if (fd.cFileName[0] == L'.' && fd.cFileName[1] == 0 ||
        fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' &&
            fd.cFileName[2] == 0) {
      // nop for "." and ".."
    } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      r = StrDbPushU16le(sb_path, fd.cFileName, 0);
      if (r != FL_OK)
        break;
      sb_path->buffer[sb_path->pos++] = L'\\';
      sb_path->buffer[sb_path->pos++] = L'*';
      sb_path->buffer[sb_path->pos] = 0;
      r = WalkDirDfs(ctx);
      StrDbRewind(sb_path, root_pos);
      if (r != FL_OK)
        break;
    } else if (ctx->callback != NULL) {
      r = StrDbPushU16le(sb_path, fd.cFileName, 0);
      if (r != FL_OK)
        break;
      r = ctx->callback(sb_path->buffer, &fd, ctx->arg);
      StrDbRewind(sb_path, root_pos);
      if (r != FL_OK)
        break;
    }
  } while (FindNextFile(find_handle, &fd));
  FindClose(find_handle);
  return r;
}

int WalkDir(const wchar_t *path,
            file_walk_cb_t callback,
            void *arg,
            allocator_t *alloc) {
  int ret = FL_OS_ERROR;
  file_walk_t ctx = {callback, arg};
  StrDbCreate(alloc, &ctx.sb_path);
  HANDLE h;
  do {
    // Open the path, either it's a file or a directory
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;

    ret = StrDbFullPath(&ctx.sb_path, h);
    if (ret != FL_OK)
      break;

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
      ret = callback(ctx.sb_path.buffer, &emu, arg);
    } else {
      // normal recursive dir routine
      ctx.sb_path.buffer[ctx.sb_path.pos++] = L'\\';
      ctx.sb_path.buffer[ctx.sb_path.pos++] = L'*';
      ctx.sb_path.buffer[ctx.sb_path.pos] = 0;
      ret = WalkDirDfs(&ctx);
    }
  } while (0);
  CloseHandle(h);
  StrDbFree(&ctx.sb_path);
  return ret;
}

int StrDbCreate(allocator_t *alloc, str_db_t *sb) {
  const str_db_t tmp = {NULL};
  *sb = tmp;
  sb->alloc = *alloc;
  sb->ex_pad = L'\n';
  sb->pad_len = 2;
  return FL_OK;
}

int StrDbFree(str_db_t *sb) {
  sb->alloc.alloc(sb->buffer, 0, sb->alloc.arg);
  return FL_OK;
}

size_t StrDbTell(str_db_t *sb) {
  return sb->pos;
}

size_t StrDbNext(str_db_t *sb, size_t pos) {
  // if no delimiter, skip to tail
  if (sb->pad_len == 0)
    return sb->pos;

  // first delimiter char is '\0'
  while (pos < sb->pos && sb->buffer[pos] != 0)
    pos++;
  if (pos == sb->pos)
    return pos;
  pos++;

  // skip second delimiter
  if (sb->pad_len > 1 && pos < sb->pos && (1 || sb->buffer[pos] == sb->ex_pad))
    pos++;

  return pos;
}

int StrDbRewind(str_db_t *sb, size_t pos) {
  if (pos < sb->pos)
    sb->pos = pos;
  if (sb->pos != sb->size && sb->buffer) {
    sb->buffer[sb->pos] = 0;
  }
  return FL_OK;
}

const wchar_t *StrDbGet(str_db_t *sb, size_t pos) {
  return sb->buffer + pos;
}

int StrDbPreAlloc(str_db_t *sb, size_t cch) {
  if (sb->pos + cch > sb->size) {
    const size_t new_size = (sb->pos + cch) * 2;
    wchar_t *new_buf = (wchar_t *)sb->alloc.alloc(
        sb->buffer, new_size * sizeof sb->buffer[0], sb->alloc.arg);
    if (new_buf == NULL)
      return FL_OUT_OF_MEMORY;
    sb->buffer = new_buf;
    sb->size = new_size;
  }
  return FL_OK;
}

int StrDbPushU16le(str_db_t *sb, const wchar_t *str, size_t cch) {
  const size_t original_cch = cch;
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
  for (size_t i = 0; i != cch; i++)
    buf[i] = str[i];
  buf[cch] = 0;
  buf[cch + 1] = sb->ex_pad;
  sb->pos += cch + sb->pad_len;
  return FL_OK;
}

int StrDbPushU16be(str_db_t *sb, const wchar_t *str, size_t cch) {
  const size_t pos = sb->pos;
  int r = StrDbPushU16le(sb, str, cch);
  if (r != FL_OK)
    return r;
  wchar_t *buf = &sb->buffer[pos];
  for (size_t i = 0; buf[i] != 0; i++) {
    buf[i] = be16(buf[i]);
  }
  return FL_OK;
}

int StrDbIsDuplicate(str_db_t *sb, size_t start, size_t target) {
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

int StrDbFullPath(str_db_t *sb, HANDLE handle) {
  int r = FL_OS_ERROR;
  sb->pos = 0;
  sb->ex_pad = 0;
  sb->pad_len = 0;
  do {
    const DWORD name_flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = GetFinalPathNameByHandle(handle, NULL, 0, name_flags);
    if (size == 0)
      break;
    sb->pos = size;
    r = StrDbPreAlloc(sb, size + MAX_PATH * 2);
    if (r != FL_OK)
      break;
    if (GetFinalPathNameByHandle(handle, sb->buffer, sb->size, name_flags) == 0)
      break;
    while (sb->pos > 0 && sb->buffer[sb->pos - 1] == 0)
      sb->pos--;
    r = FL_OK;
  } while (0);

  if (r != FL_OK)
    StrDbRewind(sb, 0);
  return r;
}

wchar_t *FlStrCpyW(wchar_t *dst, const wchar_t *src) {
  return StrCpyW(dst, src);
}

wchar_t *FlStrCpyNW(wchar_t *dst, const wchar_t *src, size_t cch) {
  return StrCpyNW(dst, src, cch);
}

size_t FlStrLenW(const wchar_t *str) {
  size_t r = 0;
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
