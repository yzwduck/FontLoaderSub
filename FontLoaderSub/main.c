#include <Windows.h>
#include <tchar.h>
#include <CommCtrl.h>
#include <Shobjidl.h>

#include "util.h"
#include "font_loader.h"
#include "path.h"
#include "mock_config.h"

#define kAppTitle L"FontLoaderSub r4"
#define kCacheFile L"fc-subs.db"

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

typedef enum {
  APP_LOAD_SUB = 0,
  APP_LOAD_CACHE,
  APP_SCAN_FONT,
  APP_LOAD_FONT,
  APP_UNLOAD_FONT,
  APP_DONE
} FL_AppState;

typedef struct {
  HINSTANCE hInst;
  allocator_t *alloc;
  int argc;
  LPWSTR *argv;

  int cancelled;
  int error;
  int req_exit;
  FL_LoaderCtx loader;
  FL_AppState app_state;
  wchar_t status_txt[128];  // should be sufficient
  str_db_t log;
  const wchar_t *font_path;
  wchar_t exe_path[MAX_PATH];

  HWND work_hwnd;
  HANDLE thread_load;
  HANDLE thread_cache;
  HANDLE evt_stop_cache;

  TASKDIALOGCONFIG dlg_work;
  TASKDIALOGCONFIG dlg_done;
  ITaskbarList3 *taskbar_list3;
} FL_AppCtx;

static void AppHelpUsage(FL_AppCtx *c, HWND hWnd) {
  TaskDialog(
      hWnd, c->hInst, kAppTitle, L"Usage",
      L"1. Move EXE to font folder,\n"
      L"2. Drop ass/ssa/folder onto EXE,\n"
      L"3. Hit \"Retry\"?",
      TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, NULL);
}

static int AppBuildLog(FL_AppCtx *c) {
  vec_t *loaded = &c->loader.loaded_font;
  str_db_t *log = &c->log;

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
    if (m->filename && !(m->flag & FL_LOAD_DUP)) {
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

static int AppUpdateStatus(FL_AppCtx *c) {
  FS_Stat stat = {0};
  if (c->loader.font_set) {
    fs_stat(c->loader.font_set, &stat);
  }
  wsprintfW(
      c->status_txt,
      L"%d loaded. %d failed. %d unmatched.\n%d files. %d fonts. %d subs.",
      c->loader.num_font_loaded, c->loader.num_font_failed,
      c->loader.num_font_unmatched, stat.num_file, stat.num_face,
      c->loader.num_sub);

  const wchar_t *cap;
  if (c->cancelled) {
    cap = L"Cancelling";
  } else {
    switch (c->app_state) {
    case APP_LOAD_SUB:
      cap = L"Subtitle";
      break;
    case APP_LOAD_CACHE:
      cap = L"Cache";
      break;
    case APP_SCAN_FONT:
      cap = L"Font";
      break;
    case APP_LOAD_FONT:
      cap = L"Load";
      break;
    case APP_UNLOAD_FONT:
      cap = L"Unload";
      break;
    default:
      cap = L"Done";
      break;
    }
  }

  SendMessage(
      c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, (LPARAM)cap);
  SendMessage(
      c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)c->status_txt);

  return 0;
}

static DWORD WINAPI AppWorker(LPVOID param) {
  FL_AppCtx *c = (FL_AppCtx *)param;
  int r = FL_OK;
  while (r == FL_OK && !c->cancelled && c->app_state != APP_DONE) {
    switch (c->app_state) {
    case APP_LOAD_SUB: {
      if (MOCK_SUB_PATH) {
        r = fl_add_subs(&c->loader, MOCK_SUB_PATH);
      }
      for (int i = 1; i < c->argc && r == FL_OK; i++) {
        r = fl_add_subs(&c->loader, c->argv[i]);
      }
      c->app_state = APP_LOAD_CACHE;
      break;
    }
    case APP_LOAD_CACHE: {
      fl_scan_fonts(&c->loader, c->font_path, kCacheFile);
      FS_Stat stat = {0};
      fs_stat(c->loader.font_set, &stat);
      if (stat.num_face == 0) {
        c->app_state = APP_SCAN_FONT;
      } else {
        c->app_state = APP_LOAD_FONT;
      }
      break;
    }
    case APP_SCAN_FONT: {
      if (fl_scan_fonts(&c->loader, c->font_path, NULL) == FL_OK) {
        fl_save_cache(&c->loader, kCacheFile);
      }
      c->app_state = APP_LOAD_FONT;
      break;
    }
    case APP_LOAD_FONT: {
      r = fl_load_fonts(&c->loader);
      if (r == FL_OK)
        c->app_state = APP_DONE;
      break;
    }
    case APP_UNLOAD_FONT: {
      fl_unload_fonts(&c->loader);
      if (c->req_exit) {
        c->cancelled = 1;
      } else {
        c->app_state = APP_SCAN_FONT;
      }
      break;
    }
    default: {
      // nop
    }
    }
  }
  if (c->cancelled)
    fl_unload_fonts(&c->loader);

  return 0;
}

static DWORD WINAPI AppCacheWorker(LPVOID param) {
  FL_AppCtx *c = (FL_AppCtx *)param;

  while (1) {
    fl_cache_fonts(&c->loader, c->evt_stop_cache);
    if (WaitForSingleObject(c->evt_stop_cache, 5 * 60 * 1000) != WAIT_TIMEOUT)
      break;
  }
  return 0;
}

static HRESULT CALLBACK DlgWorkProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_CREATED || uNotification == TDN_NAVIGATED) {
    c->work_hwnd = hWnd;
    SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
    AppUpdateStatus(c);

    DWORD thread_id;
    c->thread_load = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
    if (c->thread_load == NULL) {
      c->cancelled = 1;
    }
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL) {
      c->cancelled = 1;
      fl_cancel(&c->loader);
      // close dialog only if worker is done
      if (WaitForSingleObject(c->thread_load, 0) == WAIT_TIMEOUT)
        return S_FALSE;
      AppUpdateStatus(c);
    }
  } else if (uNotification == TDN_TIMER) {
    AppUpdateStatus(c);
    DWORD r = WaitForSingleObject(c->thread_load, 0);
    if (r != WAIT_TIMEOUT) {
      CloseHandle(c->thread_load);
      c->thread_load = NULL;
      // done, or error
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_NOPROGRESS);
      }
      if (c->app_state == APP_DONE) {
        if (AppBuildLog(c)) {
          c->dlg_done.pszExpandedInformation = str_db_get(&c->log, 0);
        } else {
          c->dlg_done.pszExpandedInformation = NULL;
        }
        c->dlg_done.pszContent = c->status_txt;
        SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_done);
      } else {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
      }
      SendMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
    } else {
      // work in progress
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_INDETERMINATE);
      }
    }
  }
  return S_OK;
}

static HRESULT CALLBACK DlgDoneProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_NAVIGATED) {
    c->thread_cache = NULL;

    FS_Stat stat = {0};
    fs_stat(c->loader.font_set, &stat);
    if (c->loader.num_sub_font == 0 || stat.num_face == 0) {
      AppHelpUsage(c, hWnd);
    } else {
      DWORD thread_id;
      ResetEvent(c->evt_stop_cache);
      c->thread_cache = CreateThread(NULL, 0, AppCacheWorker, c, 0, &thread_id);
    }
  } else if (uNotification == TDN_HYPERLINK_CLICKED) {
    ShellExecute(NULL, NULL, (LPCWSTR)lParam, NULL, NULL, SW_SHOW);
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL || wParam == IDCLOSE || wParam == IDRETRY) {
      if (wParam != IDRETRY) {
        c->req_exit = 1;
      }
      SetEvent(c->evt_stop_cache);
      if (WaitForSingleObject(c->thread_cache, 1000) != WAIT_OBJECT_0) {
        TerminateThread(c->thread_cache, 2);
      }
      CloseHandle(c->thread_cache);
      c->thread_cache = NULL;
      c->app_state = APP_UNLOAD_FONT;
      SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
      return S_FALSE;
    }
    if (wParam == IDOK) {
      ShowWindow(hWnd, SW_MINIMIZE);
      return S_FALSE;
    }
  }
  return S_OK;
}

static int AppInit(FL_AppCtx *c, HINSTANCE hInst, allocator_t *alloc) {
  c->hInst = hInst;
  c->alloc = alloc;

  c->dlg_work.cbSize = sizeof c->dlg_work;
  c->dlg_work.hInstance = hInst;
  c->dlg_work.pszWindowTitle = kAppTitle;
  c->dlg_work.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c->dlg_work.dwFlags =
      TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
  c->dlg_work.pszMainInstruction = L"";
  c->dlg_work.lpCallbackData = (LONG_PTR)c;
  c->dlg_work.pfCallback = DlgWorkProc;

  c->dlg_done.cbSize = sizeof c->dlg_work;
  c->dlg_done.hInstance = hInst;
  c->dlg_done.pszWindowTitle = kAppTitle;
  c->dlg_done.dwCommonButtons =
      TDCBF_CLOSE_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_OK_BUTTON;
  c->dlg_done.pszMainInstruction = L"Done";
  c->dlg_done.dwFlags =
      TDF_CAN_BE_MINIMIZED | TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT;
  c->dlg_done.pszFooterIcon = TD_SHIELD_ICON;
  c->dlg_done.pszFooter =
      L"GPLv2: <A HREF=\"https://github.com/yzwduck/FontLoaderSub\">"
      L"github.com/yzwduck/FontLoaderSub</A>";
  c->dlg_done.lpCallbackData = (LONG_PTR)c;
  c->dlg_done.pfCallback = DlgDoneProc;

  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);
  if (c->argv == NULL)
    return 0;
  if (GetModuleFileName(NULL, c->exe_path, MAX_PATH) == 0)
    return 0;
  if (fl_init(&c->loader, c->alloc) != FL_OK)
    return 0;
  str_db_init(&c->log, c->alloc, 0, 0);
  c->font_path = c->exe_path;

  if (MOCK_FONT_PATH)
    c->font_path = MOCK_FONT_PATH;

  c->evt_stop_cache = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (c->evt_stop_cache == NULL)
    return 0;

  if (SUCCEEDED(OleInitialize(NULL))) {
    if (SUCCEEDED(CoCreateInstance(
            &CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
            (void **)&c->taskbar_list3))) {
      if (FAILED(c->taskbar_list3->lpVtbl->HrInit(c->taskbar_list3))) {
        c->taskbar_list3->lpVtbl->Release(c->taskbar_list3);
        c->taskbar_list3 = NULL;
      }
    }
  }

  return 1;
}

static int AppRun(FL_AppCtx *c) {
  TaskDialogIndirect(&c->dlg_work, NULL, NULL, NULL);

  // clean up
  if (WaitForSingleObject(c->thread_load, 16384) == WAIT_TIMEOUT) {
    TerminateThread(c->thread_load, 1);
    fl_unload_fonts(&c->loader);
  }
  return 0;
}

FL_AppCtx g_app;

int test_main();

int WINAPI _tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpCmdLine,
    int nCmdShow) {
  int r = 0; // test_main();
  if (r) 
    return r;

  PerMonitorDpiHack();

  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t alloc = {.alloc = mem_realloc, .arg = heap};
  FL_AppCtx *ctx = &g_app;
  // FL_AppCtx *ctx = alloc.alloc(NULL, sizeof *ctx, alloc.arg);
  if (ctx == NULL || !AppInit(ctx, hInstance, &alloc)) {
    TaskDialog(
        NULL, hInstance, kAppTitle, L"Error...", NULL, TDCBF_CLOSE_BUTTON,
        TD_ERROR_ICON, NULL);
    return 1;
  }
  AppRun(ctx);

  return 0;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
