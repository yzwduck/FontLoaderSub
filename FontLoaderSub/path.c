#include "path.h"

typedef struct {
  FL_FileWalkCb callback;
  void *arg;
  str_db_t path;
} FL_WalkDirCtx;

int FlResolvePath(const wchar_t *path, str_db_t *s) {
  int r = FL_OK;
  HANDLE handle;

  do {
    handle = CreateFile(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    // return value unchecked

    const DWORD name_flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = GetFinalPathNameByHandle(handle, NULL, 0, name_flags);
    if (size == 0) {
      r = FL_OS_ERROR;
      break;
    }
    // allocate buffer
    str_db_seek(s, 0);
    const DWORD space = vec_prealloc(&s->vec, size + MAX_PATH / 2);
    if (space < size) {
      r = FL_OUT_OF_MEMORY;
      break;
    }
    // get path
    wchar_t *buffer = (wchar_t *)str_db_get(s, 0);
    const DWORD cch =
        GetFinalPathNameByHandle(handle, buffer, space, name_flags);
    if (cch == 0 || cch >= size) {
      r = FL_OS_ERROR;
      break;
    }
    s->vec.n = cch;
  } while (0);

  CloseHandle(handle);
  return r;
}

size_t FlPathParent(str_db_t *path) {
  size_t pos = str_db_tell(path);
  wchar_t *buf = (wchar_t *)str_db_get(path, 0);
  while (pos != 0 && buf[pos - 1] != L'\\')
    pos--;
  buf[pos] = 0;
  str_db_seek(path, pos);
  return pos;
}

static int WalkDirDfs(FL_WalkDirCtx *ctx) {
  int r = FL_OK;
  WIN32_FIND_DATA fd;
  HANDLE find_handle = FindFirstFile(str_db_get(&ctx->path, 0), &fd);
  if (find_handle == INVALID_HANDLE_VALUE) {
    // ignore error, recommended
    return FL_OK;
    // stop on any error
    // return FL_OS_ERROR;
  }

  const size_t pos_root = FlPathParent(&ctx->path);

  do {
    if (fd.cFileName[0] == L'.' && fd.cFileName[1] == 0 ||
        fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' &&
            fd.cFileName[2] == 0) {
      // ignore current and parent directory
    } else {
      // construct the full name
      str_db_seek(&ctx->path, pos_root);
      const wchar_t *filename =
          str_db_push_u16_le(&ctx->path, fd.cFileName, MAX_PATH);
      if (filename == NULL) {
        r = FL_OUT_OF_MEMORY;
        break;
      }
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // it's a directory, append \*
        const wchar_t *search = str_db_push_u16_le(&ctx->path, L"\\*", 2);
        if (search == NULL) {
          r = FL_OUT_OF_MEMORY;
          break;
        }
        r = WalkDirDfs(ctx);
      } else {
        // it's a file, fire callback
        const wchar_t *full = str_db_get(&ctx->path, 0);
        r = ctx->callback(full, &fd, ctx->arg);
      }
    }
  } while (r == FL_OK && FindNextFile(find_handle, &fd));

  FindClose(find_handle);
  return r;
}

int FlWalkDir(
    const wchar_t *path,
    allocator_t *alloc,
    FL_FileWalkCb callback,
    void *arg) {
  int r;
  FL_WalkDirCtx ctx = {.callback = callback, .arg = arg};
  str_db_init(&ctx.path, alloc, 0, 0);

  do {
    const wchar_t *a = str_db_push_u16_le(&ctx.path, path, 0);
    if (a == NULL) {
      r = FL_OUT_OF_MEMORY;
      break;
    }

    r = WalkDirDfs(&ctx);
  } while (0);

  str_db_free(&ctx.path);
  return r;
}

int FlWalkDirStr(str_db_t *path, FL_FileWalkCb callback, void *arg) {
  // assume path->pad_len == 0
  FL_WalkDirCtx ctx = {.callback = callback, .arg = arg, .path = *path};
  const int r = WalkDirDfs(&ctx);
  *path = ctx.path;
  return r;
}
