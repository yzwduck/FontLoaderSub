#include <Windows.h>
#include <tchar.h>
#include "util.h"
#include "font_loader.h"
#include "path.h"

static void *mem_realloc(void *existing, size_t size, void *arg) {
  HANDLE heap = (HANDLE)arg;
  if (size == 0) {
    HeapFree(heap, 0, existing);
    return NULL;
  }
  if (existing == NULL) {
    return HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
  }
  return HeapReAlloc(heap, HEAP_ZERO_MEMORY, existing, size);
}

static int AppBuildLog(vec_t *loaded, str_db_t *log) {
  str_db_seek(log, 0);
  FL_FontMatch *data = loaded->data;

  for (size_t i = 0; i != loaded->n; i++) {
    const wchar_t *tag;
    FL_FontMatch *m = &data[i];
    if (m->flag & (FL_OS_LOADED | FL_LOAD_OK))
      tag = L"[ok] ";
    else if (m->flag & (FL_LOAD_ERR))
      tag = L"[ X] ";
    else if (m->flag & (FL_LOAD_DUP))
      tag = L"[^ ] ";
    else if (1 || m->flag & (FL_LOAD_MISS))
      tag = L"[??] ";
    if (!str_db_push_u16_le(log, tag, 0) ||
        !str_db_push_u16_le(log, m->face, 0))
      return 0;
    if (m->filename) {
      if (!str_db_push_u16_le(log, L" > ", 0) ||
          !str_db_push_u16_le(log, m->filename, 0))
        return 0;
    }
    if (!str_db_push_u16_le(log, L"\n", 0))
      return 0;
  }
  const size_t pos = str_db_tell(log);
  if (!pos)
    return 0;
  wchar_t *buf = (wchar_t *)str_db_get(log, 0);
  buf[pos - 1] = 0;
  return 1;
}

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR lpCmdLine,
                     int nCmdShow) {
  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t allocator = {.alloc = mem_realloc, .arg = heap};
  str_db_t log;
  str_db_init(&log, &allocator, 0, 0);

  wchar_t buf[MAX_PATH];
  GetModuleFileName(NULL, buf, MAX_PATH);
  FL_LoaderCtx c;
  fl_init(&c, &allocator);
  fl_scan_fonts(&c, L"E:\\Data\\Fonts", NULL);
  fl_save_cache(&c, L"fc-subs.db");
  // fl_add_subs(&c, L"E:\\Data\\Subs");

  // fl_load_fonts(&c);
  // fl_unload_fonts(&c);

  return 0;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
