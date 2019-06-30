#include <Windows.h>
#include <tchar.h>
#include <CommCtrl.h>
#include <Shobjidl.h>

#include "util.h"
#include "font_loader.h"
#include "path.h"
#include "shortcut.h"
#include "mock_config.h"
#include "res/resource.h"

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
  APP_LOAD_SUB = IDS_WORK_SUBTITLE,
  APP_LOAD_CACHE = IDS_WORK_CACHE,
  APP_SCAN_FONT = IDS_WORK_FONT,
  APP_LOAD_FONT = IDS_WORK_LOAD,
  APP_UNLOAD_FONT = IDS_WORK_UNLOAD,
  APP_DONE = IDS_WORK_DONE,
  APP_CANCELLED
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
  wchar_t status_txt[256];  // should be sufficient
  str_db_t log;
  const wchar_t *font_path;
  // wchar_t exe_path[MAX_PATH];
  str_db_t full_exe_path;

  HWND work_hwnd;
  HANDLE thread_load;
  HANDLE thread_cache;
  HANDLE evt_stop_cache;

  TASKDIALOGCONFIG dlg_work;
  TASKDIALOGCONFIG dlg_done;
  TASKDIALOGCONFIG dlg_help;
  int show_shortcut;
  FL_ShortCtx shortcut;
  ITaskbarList3 *taskbar_list3;
} FL_AppCtx;

static void AppHelpUsage(FL_AppCtx *c, HWND hWnd) {
  c->show_shortcut = 0;
  c->dlg_help.hwndParent = hWnd;
  TaskDialogIndirect(&c->dlg_help, NULL, NULL, NULL);
  if (c->show_shortcut) {
    ShortcutShow(&c->shortcut, hWnd);
  }
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
    else if (m->flag & (FL_LOAD_DUP))
      tag = L"[^ ] ";
    else if (m->flag & (FL_LOAD_ERR))
      tag = L"[ X] ";
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

  DWORD_PTR args[] = {
      // arguments
      c->loader.num_font_loaded,
      c->loader.num_font_failed,
      c->loader.num_font_unmatched,
      stat.num_file,
      stat.num_face,
      c->loader.num_sub,
  };
  FormatMessage(
      FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
      ResLoadString(c->hInst, IDS_LOAD_STAT), 0, 0, c->status_txt,
      _countof(c->status_txt), (va_list *)args);

  LPARAM cap_id;
  if (c->cancelled || c->app_state == APP_CANCELLED) {
    cap_id = IDS_WORK_CANCELLING;
  } else {
    cap_id = c->app_state;
  }

  SendMessage(c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, cap_id);
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
  if (c->cancelled) {
    fl_unload_fonts(&c->loader);
    c->app_state = APP_CANCELLED;
  }

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
  int navigated = 0;
  if (uNotification == TDN_CREATED || uNotification == TDN_NAVIGATED) {
    c->work_hwnd = hWnd;
    SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);

    DWORD thread_id;
    c->thread_load = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
    if (c->thread_load == NULL) {
      // fatal error, try exit early
      c->cancelled = 1;
      c->app_state = APP_CANCELLED;
      PostMessage(hWnd, WM_CLOSE, 0, 0);
    }
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL) {
      if (c->app_state == APP_CANCELLED) {
        // SendMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
        return S_OK;  // exit cleared
      }
      if (!c->req_exit) {
        c->cancelled = 1;
        fl_cancel(&c->loader);  // signal cancel event
      }
    }
  } else if (uNotification == TDN_TIMER) {
    DWORD r = WaitForSingleObject(c->thread_load, 0);
    if (r != WAIT_TIMEOUT) {
      // worker exited
      CloseHandle(c->thread_load);
      c->thread_load = NULL;
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_NOPROGRESS);
      }
      if (c->app_state == APP_DONE) {
        // worker exited without error...
        if (!c->cancelled) {
          // and has not been cancelled
          if (AppBuildLog(c)) {
            c->dlg_done.pszExpandedInformation = str_db_get(&c->log, 0);
          } else {
            c->dlg_done.pszExpandedInformation = NULL;
          }
          AppUpdateStatus(c);
          c->dlg_done.pszContent = c->status_txt;
          // SendMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_done);
          navigated = 1;
        } else {
          // worker done, then cancelled before timer,
          // work again to continue cancellation routine
          c->app_state = APP_UNLOAD_FONT;
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      } else {
        if (c->app_state != APP_CANCELLED) {
          // it's an error
          TaskDialog(
              hWnd, c->hInst, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...",
              NULL, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
        }
        PostMessage(hWnd, WM_CLOSE, 0, 0);
      }
    } else {
      // work in progress
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_INDETERMINATE);
      }
    }
  }
  if (!navigated)
    AppUpdateStatus(c);
  return S_FALSE;
}

static HRESULT CALLBACK DlgHelpProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_HYPERLINK_CLICKED) {
    PostMessage(hWnd, WM_CLOSE, 0, 0);
    c->show_shortcut = 1;
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
  c->dlg_work.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->dlg_work.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c->dlg_work.dwFlags =
      TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
  c->dlg_work.pszMainInstruction = L"";
  c->dlg_work.lpCallbackData = (LONG_PTR)c;
  c->dlg_work.pfCallback = DlgWorkProc;

  c->dlg_done.cbSize = sizeof c->dlg_done;
  c->dlg_done.hInstance = hInst;
  c->dlg_done.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->dlg_done.dwCommonButtons =
      TDCBF_CLOSE_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_OK_BUTTON;
  c->dlg_done.pszMainInstruction = MAKEINTRESOURCE(IDS_WORK_DONE);
  c->dlg_done.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS |
                        TDF_SIZE_TO_CONTENT;
  c->dlg_done.pszFooterIcon = TD_SHIELD_ICON;
  c->dlg_done.pszFooter = MAKEINTRESOURCE(IDS_LICENSE);
  c->dlg_done.lpCallbackData = (LONG_PTR)c;
  c->dlg_done.pfCallback = DlgDoneProc;

  c->dlg_help.cbSize = sizeof c->dlg_help;
  c->dlg_help.hInstance = hInst;
  c->dlg_help.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->dlg_help.pszMainIcon = TD_INFORMATION_ICON;
  c->dlg_help.pszMainInstruction = MAKEINTRESOURCE(IDS_HELP);
  c->dlg_help.pszContent = MAKEINTRESOURCE(IDS_USAGE);
  c->dlg_help.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  c->dlg_help.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c->dlg_help.lpCallbackData = (LONG_PTR)c;
  c->dlg_help.pfCallback = DlgHelpProc;

  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);
  if (c->argv == NULL)
    return 0;
  if (str_db_init(&c->full_exe_path, c->alloc, 0, 0))
    return 0;

  DWORD initial = MAX_PATH;
  while (1) {
    if (vec_prealloc(&c->full_exe_path.vec, initial) < initial)
      return 0;
    DWORD ret = GetModuleFileName(
        NULL, (WCHAR *)str_db_get(&c->full_exe_path, 0), initial);
    if (ret == 0)
      return 0;
    if (ret < initial) {
      // sufficient buffer size
      break;
    } else {
      initial = initial * 2;
    }
  }
  if (str_db_push_u16_le(
          &c->full_exe_path, str_db_get(&c->full_exe_path, 0), 0) == NULL)
    return 0;
  ShortcutInit(&c->shortcut, hInst, c->alloc);
  c->shortcut.key = L"FontLoaderSub";  // registry key
  c->shortcut.dlg_title = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->shortcut.dir_bg_menu_str_id = IDS_SHELL_VERB;
  c->shortcut.sendto_str_id = IDS_SENDTO;
  c->shortcut.path = str_db_get(&c->full_exe_path, 0);
  c->app_state = APP_LOAD_SUB;
  if (fl_init(&c->loader, c->alloc) != FL_OK)
    return 0;
  str_db_init(&c->log, c->alloc, 0, 0);
  c->font_path = str_db_get(&c->full_exe_path, 0);

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
  if (GetAsyncKeyState(VK_SHIFT)) {
    ShortcutShow(&c->shortcut, NULL);
    return 0;
  }

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
        NULL, hInstance, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...", NULL,
        TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
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
