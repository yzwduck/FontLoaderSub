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

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR lpCmdLine,
                     int nCmdShow) {
  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t allocator = {.alloc = mem_realloc, .arg = heap};

  wchar_t buf[MAX_PATH];
  GetModuleFileName(NULL, buf, MAX_PATH);
  FL_LoaderCtx c;
  fl_init(&c, &allocator);
  // fl_add_subs(&c, L"E:\\_processing\\");
  fl_scan_fonts(&c, L"E:\\Data\\Fonts", NULL);
  fl_save_cache(&c, L"fc-subs.db");

  return 0;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
