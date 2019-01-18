#include <Windows.h>
#include <tchar.h>
#include <CommCtrl.h>

#include "util.h"
#include "font_loader.h"
#include "path.h"

#define kAppTitle L"FontLoaderSub r4"

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

typedef struct {
  HINSTANCE hInst;
  int argc;
  LPWSTR *argv;

  str_db_t *log;

  TASKDIALOGCONFIG dlg_work;
  TASKDIALOGCONFIG dlg_done;
} FL_AppCtx;

static int AppInit(FL_AppCtx *c, HINSTANCE hInst) {
  c->dlg_work.cbSize = sizeof c->dlg_work, c->dlg_work.hInstance = hInst,
  c->dlg_work.pszWindowTitle = kAppTitle,
  c->dlg_work.dwCommonButtons = TDCBF_CANCEL_BUTTON,
  c->dlg_work.dwFlags =
      TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT,
  c->dlg_work.pszMainInstruction = L"Look here";

  c->dlg_done.cbSize = sizeof c->dlg_work, c->dlg_done.hInstance = hInst,
  c->dlg_done.pszWindowTitle = kAppTitle,
  c->dlg_done.dwCommonButtons =
      TDCBF_CLOSE_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_OK_BUTTON,
  c->dlg_done.pszMainInstruction = L"Done",
  c->dlg_done.dwFlags =
      TDF_CAN_BE_MINIMIZED | TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT,
  c->dlg_done.pszFooterIcon = TD_SHIELD_ICON,
  c->dlg_done.pszFooter =
      L"GPLv2: <A HREF=\"https://github.com/yzwduck/FontLoaderSub\">"
      L"github.com/yzwduck/FontLoaderSub</A>";
  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);

  // TaskDialogIndirect(&c->dlg_done, NULL, NULL, NULL);
  return 1;
}

static void AppHelpUsage(FL_AppCtx *c, HWND hWnd) {
  TaskDialog(
      hWnd, c->hInst, kAppTitle, L"Usage",
      L"1. Move EXE to font folder,\n"
      L"2. Drop ass/ssa/folder onto EXE,\n"
      L"3. Hit \"Retry\"?",
      TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, NULL);
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

FL_AppCtx g_app;

int WINAPI _tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpCmdLine,
    int nCmdShow) {
  PerMonitorDpiHack();

  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t alloc = {.alloc = mem_realloc, .arg = heap};
  FL_AppCtx *ctx = &g_app;
  // FL_AppCtx *ctx = alloc.alloc(NULL, sizeof *ctx, alloc.arg);
  if (ctx == NULL || !AppInit(ctx, hInstance)) {
    // Error
    return 1;
  }
  // AppHelpUsage(ctx, NULL);
  str_db_t log;
  str_db_init(&log, &alloc, 0, 0);

  wchar_t buf[MAX_PATH];
  GetModuleFileName(NULL, buf, MAX_PATH);
  FL_LoaderCtx c;
  const wchar_t *cache_fn = L"fc-subs.db";
  fl_init(&c, &alloc);
  if (1 || fl_scan_fonts(&c, L"E:\\Data\\Fonts", cache_fn) != FL_OK) {
    if (fl_scan_fonts(&c, L"E:\\Data\\Fonts", NULL) != FL_OK) {
      FlBreak();
    }
    // fl_save_cache(&c, cache_fn);
  }
  fl_add_subs(&c, L"E:\\Data\\Subs");

  fl_load_fonts(&c);
  AppBuildLog(&c.loaded_font, &log);
  fl_unload_fonts(&c);

  return 0;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
