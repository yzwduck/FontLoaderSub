#include <Windows.h>
#include <tchar.h>
#include "util.h"
#include "ass_parser.h"

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
  allocator_t alo = {.alloc = mem_realloc, .arg = heap};
  memmap_t mm;
  FlMemMap(L"E:\\_processing\\test_01.ass", &mm);
  size_t cch;
  wchar_t *txt = FlTextDecode(mm.data, mm.size, &cch, &alo);
  ass_process_data(txt, cch, NULL, NULL);
  return 0;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
