#include <Windows.h>
#include <CommCtrl.h>
#include <stdint.h>
#include <tchar.h>
#include <wincrypt.h>
#include <assert.h>

#include "font_set.h"
#include "ssa_parser.h"

#define kAppTitle L"FontLoaderSub r3"
#define kFontStatusOK L"[ok] "
#define kFontStatusErr L"[ X] "
#define kFontStatus404 L"[??] "
#define kFontStatusDup L"[^ ] "

#if 0
#define MOCK_FAKE_LOAD 1
#define MOCK_NO_SYS 1
#define MOCK_FONT_DIR L"C:\\path_to_fonts"
#define MOCK_SUBS_DIR L"C:\\path_to_subs"
#else
#define MOCK_FAKE_LOAD 0
#define MOCK_NO_SYS 0
#define MOCK_FONT_DIR NULL
#define MOCK_SUBS_DIR NULL
#endif

enum APP_STATE {
  APP_IDLE = 0,
  APP_PARSE_ASS,
  APP_LOAD_CACHE,
  APP_LOAD_FONT,
  APP_LOAD_RES,
  APP_DONE,
  APP_UNLOAD_RES
};

typedef struct {
  TASKDIALOGCONFIG dlg_work;
  TASKDIALOGCONFIG dlg_done;

  allocator_t alloc;
  int argc;
  LPWSTR *argv;
  enum APP_STATE app_state;
  enum APP_STATE last_state;
  int cancelled;
  int req_exit;
  int error_env;
  const wchar_t *db_path;

  // subs
  str_db_t sub_font;
  uint32_t num_sub_font;
  uint32_t num_font_loaded;
  uint32_t num_font_failed;
  uint32_t num_font_unmatch;
  uint32_t num_subs;

  // font
  font_set_t *font_set;
  bset_t bset_file_id;
  bset_t bset_file_hash;
  HCRYPTPROV cypt_prov;  // used for hashing

  // worker
  HANDLE thread;
  HWND hWnd;

  const wchar_t *main_action;
  wchar_t buffer[96];
  str_db_t root_path;
  size_t root_pos;
  str_db_t log;
} app_ctx_t;

static const wchar_t *kFontWeightSuffix[] = {L"normal", L"regular", L"medium"};

// callback for saving font names in subtitles
static int font_name_cb(const wchar_t *font, size_t cch, void *arg) {
  app_ctx_t *ctx = (app_ctx_t *)arg;

  if (font[0] == L'@') {
    // skip the '@' char
    font++;
    cch--;
  }

  // disabled, results less matching
  for (int i = 0; 0 && i != _countof(kFontWeightSuffix); i++) {
    const wchar_t *sfx = kFontWeightSuffix[i];
    size_t len_sfx = FlStrLenW(sfx);
    if (cch > len_sfx && FlStrCmpNIW(sfx, &font[cch - len_sfx], len_sfx) == 0) {
      cch -= len_sfx;
      if (cch > 0 && font[cch - 1] == L' ')
        cch--;
      break;
    }
  }

  // add font to the set
  str_db_t *sb = &ctx->sub_font;
  uint32_t pos = StrDbTell(sb);
  StrDbPushU16le(sb, font, cch);
  // wprintf(L"  font: %s\n", StrDbGet(sb, pos));
  if (StrDbIsDuplicate(sb, 0, pos)) {
    StrDbRewind(sb, pos);
  } else {
    const wchar_t *face = StrDbGet(sb, pos);
    ctx->num_sub_font++;
  }
  return 0;
}

// callback for searching subtitle files
static int walk_cb_sub(const wchar_t *full_path,
                       WIN32_FIND_DATA *data,
                       void *arg) {
  const wchar_t *eos = full_path + FlStrLenW(full_path);
  app_ctx_t *ctx = (app_ctx_t *)arg;
  if (ctx->cancelled)
    return 1;

  int ext_ass =
      FlStrCmpIW(eos - 4, L".ass") == 0 || FlStrCmpIW(eos - 4, L".ssa") == 0;
  if (!ext_ass || data->nFileSizeHigh > 0 ||
      data->nFileSizeLow > 64 * 1024 * 1024)
    return 0;

  wchar_t *txt = NULL;
  size_t cch;
  do {
    // load subtitle and parse fonts
    txt = TextFileFromPath(full_path, &cch, &ctx->alloc);
    if (txt == NULL)
      break;
    AssParseFont(txt, cch, font_name_cb, arg);
    ctx->num_subs++;
  } while (0);

  // free the memory
  ctx->alloc.alloc(txt, 0, ctx->alloc.arg);
  return 0;
}

// callback for searching font files
static int walk_cb_font(const wchar_t *full_path,
                        WIN32_FIND_DATA *data,
                        void *arg) {
  int r = 1;
  const wchar_t *eos = full_path + FlStrLenW(full_path);
  app_ctx_t *ctx = (app_ctx_t *)arg;
  if (ctx->cancelled)
    return 1;

  int ext_font = FlStrCmpIW(eos - 4, L".ttc") == 0 ||
                 FlStrCmpIW(eos - 4, L".ttf") == 0 ||
                 FlStrCmpIW(eos - 4, L".otf") == 0;
  if (!ext_font)
    return 0;

  memmap_t mmap;
  FlMemMap(full_path, &mmap);
  if (mmap.data) {
    r = FontSetAdd(ctx->font_set, full_path + ctx->root_pos, mmap.data,
                   mmap.size);
    FlMemUnmap(&mmap);
  }
  return 0;
}

// save results to c->root_path.buffer
static const wchar_t *AppMakeFontFullPath(app_ctx_t *c, const wchar_t *file) {
  if (!file)
    return NULL;
  StrDbRewind(&c->root_path, c->root_pos);
  if (StrDbPushU16le(&c->root_path, file, 0) == 0) {
    return c->root_path.buffer;
  } else {
    return NULL;
  }
}

static int AppHasFileId(app_ctx_t *c, uint32_t id) {
  uint32_t *fid = (uint32_t *)(c->bset_file_id.buf);
  size_t i;
  for (i = 0; i != c->bset_file_id.count; i++) {
    if (fid[i] == id)
      break;
  }
  return i != c->bset_file_id.count;
}

static int AppHasFileHash(app_ctx_t *c, uint8_t hash[32]) {
  size_t i, j;
  for (j = 0; j != c->bset_file_hash.count; j++) {
    for (i = 0; i != 32; i++) {
      if (c->bset_file_hash.buf[j * 32 + i] != hash[i])
        break;
    }
    if (i == 32)
      break;
  }
  return j != c->bset_file_hash.count;
}

static void AppReportFont(app_ctx_t *c,
                          const wchar_t *tag,
                          const wchar_t *face,
                          const wchar_t *file) {
  StrDbPushU16le(&c->log, tag, 0);
  StrDbPushU16le(&c->log, face, 0);
  if (file) {
    StrDbPushU16le(&c->log, L" > ", 0);
    StrDbPushU16le(&c->log, file, 0);
  }
  StrDbPushU16le(&c->log, L"\n", 0);
}

static int AppLoadFontFile(app_ctx_t *c,
                           const wchar_t *face,
                           const wchar_t *file) {
  int r = FL_OK;
  if (file == NULL) {
    r = FL_OS_ERROR;
  } else if (MOCK_FAKE_LOAD) {
    r = FL_OK;
    Sleep(500);
  } else if (AddFontResource(c->root_path.buffer) != 0) {
    r = FL_OK;
  } else {
    r = FL_OS_ERROR;
  }
  return r;
}

// return value: number of loaded font
static int AppLoadFontIter(app_ctx_t *c, const wchar_t *face, font_iter_t *it) {
  int r;
  // check file id
  if (AppHasFileId(c, it->file_id))
    return FL_DUP;

  BsetAdd(&c->bset_file_id, (uint8_t *)&it->file_id);
  const wchar_t *fpath = AppMakeFontFullPath(c, it->filename);
  if (fpath == NULL)
    return FL_OUT_OF_MEMORY;

  // calc SHA256
  HCRYPTPROV hasher = 0;
  if (!CryptCreateHash(c->cypt_prov, CALG_SHA_256, 0, 0, &hasher))
    return FL_OS_ERROR;
  memmap_t mmap;
  FlMemMap(c->root_path.buffer, &mmap);
  if (!mmap.data) {
    CryptDestroyHash(hasher);
    return FL_OS_ERROR;
  }
  CryptHashData(hasher, mmap.data, (DWORD)mmap.size, 0);
  FlMemUnmap(&mmap);
  DWORD cbHash = 32;
  uint8_t hash[32];
  CryptGetHashParam(hasher, HP_HASHVAL, hash, &cbHash, 0);
  CryptDestroyHash(hasher);

  if (AppHasFileHash(c, hash))
    return FL_DUP;

  // new file
  BsetAdd(&c->bset_file_hash, hash);
  return AppLoadFontFile(c, face, it->filename);
}

static int CALLBACK enum_fonts(const LOGFONTW *lfp,
                               const TEXTMETRICW *tmp,
                               DWORD fontType,
                               LPARAM lParam) {
  int *r = (int *)lParam;
  *r = 1;
  return 1;  // continue
}

static int IsFontInstalled(const wchar_t *face) {
  if (MOCK_NO_SYS)
    return 0;
  int found = 0;
  HDC dc = GetDC(0);
  EnumFontFamilies(dc, face, enum_fonts, (LPARAM)&found);
  ReleaseDC(0, dc);
  return found;
}

static void AppLoadFont(app_ctx_t *c) {
  for (uint32_t i = 0, pos = 0; i != c->num_sub_font; i++) {
    const wchar_t *face = StrDbGet(&c->sub_font, pos);
    pos = StrDbNext(&c->sub_font, pos);

    if (IsFontInstalled(face)) {
      AppReportFont(c, kFontStatusOK, face, NULL);
    } else {
      font_iter_t it;
      int num_loaded = 0;
      int num_dup = 0;
      int num_failed = 0;
      if (!FontSetLookupIter(c->font_set, face, &it)) {
        AppReportFont(c, kFontStatus404, face, NULL);
        c->num_font_unmatch++;
      } else {
        do {
          int r = AppLoadFontIter(c, face, &it);
          if (r == FL_DUP) {
            num_dup++;
          } else if (r == FL_OK) {
            num_loaded++;
            c->num_font_loaded++;
            AppReportFont(c, kFontStatusOK, face, it.filename);
          } else {
            num_failed++;
            c->num_font_failed++;
            AppReportFont(c, kFontStatusErr, face, it.filename);
          }
        } while (!c->cancelled && num_loaded <= 16 &&
                 FontSetLookupIterNext(&it));
      }
      if (num_loaded == 0 && num_failed == 0 && num_dup > 0) {
        AppReportFont(c, kFontStatusDup, face, NULL);
      }
    }
    if (c->cancelled)
      break;
  }
}

static void AppUnloadFonts(app_ctx_t *c) {
  if (c->font_set) {
    uint32_t *fid = (uint32_t *)c->bset_file_id.buf;
    for (size_t i = 0; i != c->bset_file_id.count; i++) {
      const wchar_t *file = FontSetLookupFileId(c->font_set, fid[i]);
      const wchar_t *fpath = AppMakeFontFullPath(c, file);
      if (fpath)
        RemoveFontResource(fpath);
    }
    for (uint32_t i = 0, pos = 0; i != c->num_sub_font; i++) {
      const wchar_t *face = StrDbGet(&c->sub_font, pos);
      pos = StrDbNext(&c->sub_font, pos);
      font_iter_t it;
      if (FontSetLookupIter(c->font_set, face, &it)) {
        do {
          if (!AppHasFileId(c, it.file_id)) {
            BsetAdd(&c->bset_file_id, (uint8_t *)&it.file_id);
            const wchar_t *fpath = AppMakeFontFullPath(c, it.filename);
            if (fpath)
              RemoveFontResource(fpath);
          }
        } while (FontSetLookupIterNext(&it));
      }
    }
    FontSetFree(c->font_set);
    c->font_set = NULL;
  }
  c->num_font_loaded = 0;
  c->num_font_failed = 0;
  c->num_font_unmatch = 0;
  c->num_subs = 0;
  StrDbRewind(&c->log, 0);
  BsetClear(&c->bset_file_hash);
  BsetClear(&c->bset_file_id);
}

static void AppChdir(app_ctx_t *c) {
  WCHAR path[MAX_PATH];
  GetModuleFileName(NULL, path, MAX_PATH);
  int last = 0;
  for (int i = 0; i < MAX_PATH && path[i]; i++) {
    if (path[i] == L'\\')
      last = i;
  }
  path[last + 1] = 0;
  SetCurrentDirectory(path);
  if (MOCK_FONT_DIR)
    SetCurrentDirectory(MOCK_FONT_DIR);

  HANDLE h;
  int ret = FL_OS_ERROR;
  do {
    h = CreateFile(L".", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;
    ret = StrDbFullPath(&c->root_path, h);
    if (ret != FL_OK)
      break;
    str_db_t *sb = &c->root_path;
    if (sb->buffer[sb->pos - 1] != L'\\') {
      sb->buffer[sb->pos++] = L'\\';
    }
    c->root_pos = StrDbTell(&c->root_path);
    ret = FL_OK;
  } while (0);

  if (ret != FL_OK) {
    // really bad
    ExitProcess(1);

    StrDbRewind(&c->root_path, 0);
    StrDbPushU16le(&c->root_path, path, 0);
    c->root_pos = last + 1;
  }
  CloseHandle(h);
}

static void AppUpdateStatus(app_ctx_t *c) {
  font_set_stat_t stat = {0};
  if (c->font_set) {
    FontSetStat(c->font_set, &stat);
  }
  wsprintfW(c->buffer,
            L"%d loaded. %d failed. %d unmatch.\n%d files. %d fonts. %d subs.",
            c->num_font_loaded, c->num_font_failed, c->num_font_unmatch,
            stat.num_files, stat.num_faces, c->num_subs);

  const wchar_t *cap = NULL;
  switch (c->app_state) {
  case APP_PARSE_ASS:
    cap = L"Subtitle";
    break;
  case APP_LOAD_CACHE:
    cap = L"Cache";
    break;
  case APP_LOAD_FONT:
    cap = L"Font";
    break;
  case APP_LOAD_RES:
    cap = L"Load";
    break;
  case APP_UNLOAD_RES:
    cap = L"Unload";
    break;
  default:
    cap = L"?";
    break;
  }
  c->main_action = cap;
}

static DWORD WINAPI AppWorker(LPVOID param) {
  app_ctx_t *c = (app_ctx_t *)param;
  int r = 0;
  while (r == 0 && !c->cancelled && c->app_state != APP_DONE) {
    switch (c->app_state) {
    case APP_PARSE_ASS:
      for (int i = 1; i < c->argc; i++) {
        r = WalkDir(c->argv[i], walk_cb_sub, c, &c->alloc);
        if (r != 0)
          break;
      }
      if (r != 0)
        break;
      if (MOCK_SUBS_DIR)
        WalkDir(MOCK_SUBS_DIR, walk_cb_sub, c, &c->alloc);
      AppChdir(c);
      c->app_state = APP_LOAD_CACHE;
      break;
    case APP_LOAD_CACHE:
      FontSetFree(c->font_set);
      c->font_set = NULL;
      if (FontSetLoad(c->db_path, &c->alloc, &c->font_set) == 0) {
        font_set_stat_t stat;
        FontSetStat(c->font_set, &stat);
        if (stat.num_faces > 0) {
          c->app_state = APP_LOAD_RES;
          break;
        }
      }
      FontSetFree(c->font_set);
      c->font_set = NULL;
      c->app_state = APP_LOAD_FONT;
      break;
    case APP_LOAD_FONT:
      FontSetFree(c->font_set);
      r = FontSetCreate(&c->alloc, &c->font_set);
      if (r != 0)
        break;
      StrDbRewind(&c->root_path, c->root_pos);
      r = WalkDir(c->root_path.buffer, walk_cb_font, c, &c->alloc);
      if (r != 0)
        break;
      FontSetDump(c->font_set, c->db_path);
      FontSetBuildIndex(c->font_set);
      c->app_state = APP_LOAD_RES;
      break;
    case APP_LOAD_RES:
      AppLoadFont(c);
      c->app_state = APP_DONE;
      AppUpdateStatus(c);
      break;
    case APP_UNLOAD_RES:
      AppUnloadFonts(c);
      if (c->req_exit)
        c->cancelled = 1;
      c->app_state = APP_LOAD_FONT;
    default: {}
    }
  }
  return 0;
}

static HRESULT CALLBACK DlgWorkProc(HWND hWnd,
                                    UINT uNotification,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    LONG_PTR dwRefData) {
  app_ctx_t *c = (app_ctx_t *)dwRefData;
  if (uNotification == TDN_CREATED || uNotification == TDN_NAVIGATED) {
    AppUpdateStatus(c);
    SendMessage(hWnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION,
                (LPARAM)c->main_action);
    SendMessage(hWnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)c->buffer);
    SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
    c->hWnd = hWnd;

    DWORD thread_id;
    c->thread = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
  }
  if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL) {
      c->cancelled = 1;
      if (WaitForSingleObject(c->thread, 0) == WAIT_TIMEOUT)
        return S_FALSE;
    }
  }
  if (uNotification == TDN_TIMER) {
    DWORD r = WaitForSingleObject(c->thread, 0);
    if (r != WAIT_TIMEOUT) {
      CloseHandle(c->thread);
      c->thread = NULL;
      if (c->app_state == APP_DONE) {
        if (StrDbTell(&c->log) != 0) {
          size_t pos = StrDbTell(&c->log);
          StrDbRewind(&c->log, pos - 1);
          c->dlg_done.pszExpandedInformation = StrDbGet(&c->log, 0);
        } else {
          c->dlg_done.pszExpandedInformation = NULL;
        }
        SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_done);
      } else {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
      }
    } else {
      AppUpdateStatus(c);
      SendMessage(hWnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION,
                  (LPARAM)c->main_action);
      SendMessage(hWnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)c->buffer);
    }
  }
  return S_OK;
}

static HRESULT CALLBACK DlgDoneProc(HWND hWnd,
                                    UINT uNotification,
                                    WPARAM wParam,
                                    LPARAM lParam,
                                    LONG_PTR dwRefData) {
  app_ctx_t *c = (app_ctx_t *)dwRefData;
  if (uNotification == TDN_NAVIGATED) {
    font_set_stat_t stat = {0};
    if (c->font_set) {
      FontSetStat(c->font_set, &stat);
    }
    if (c->num_sub_font == 0 || stat.num_faces == 0) {
      TaskDialog(hWnd, c->dlg_done.hInstance, kAppTitle, L"Usage",
                 L"1. Move EXE to font folder,\n"
                 L"2. Drop ass/ssa/folder onto EXE,\n"
                 L"3. Hit \"Retry\"?",
                 TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, NULL);
    }
  } else if (uNotification == TDN_HYPERLINK_CLICKED) {
    ShellExecute(NULL, NULL, (LPCWSTR)lParam, NULL, NULL, SW_SHOW);
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL || wParam == IDCLOSE || wParam == IDRETRY) {
      if (wParam != IDRETRY) {
        c->req_exit = 1;
      }
      c->app_state = APP_UNLOAD_RES;
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

int AppInit(app_ctx_t *c, HINSTANCE hInst) {
  c->alloc.alloc = mem_realloc;
  c->alloc.arg = HeapCreate(0, 0, 0);
  if (c->alloc.arg == NULL)
    return 2;
  c->db_path = L"fc-subs.db";
  c->app_state = APP_PARSE_ASS;

  StrDbCreate(&c->alloc, &c->root_path);
  StrDbCreate(&c->alloc, &c->log);
  c->root_path.pad_len = 0;
  c->log.pad_len = 0;
  if (StrDbPreAlloc(&c->root_path, MAX_PATH) != 0)
    return 1;
  if (StrDbPreAlloc(&c->log, 1024) != 0)
    return 1;

  AppUpdateStatus(c);
  c->dlg_work.cbSize = sizeof c->dlg_work;
  c->dlg_work.hInstance = hInst;
  c->dlg_work.pszWindowTitle = kAppTitle;
  c->dlg_work.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  c->dlg_work.pfCallback = DlgWorkProc;
  c->dlg_work.dwFlags =
      TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
  c->dlg_done.cxWidth = 256 * 0;
  c->dlg_work.lpCallbackData = (LONG_PTR)c;
  c->dlg_work.pszMainInstruction = c->main_action;
  c->dlg_work.pszContent = c->buffer;

  c->dlg_done.cbSize = sizeof c->dlg_done;
  c->dlg_done.hInstance = hInst;
  c->dlg_done.pszWindowTitle = c->dlg_work.pszWindowTitle;
  c->dlg_done.dwCommonButtons =
      TDCBF_CLOSE_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_OK_BUTTON;
  c->dlg_done.pfCallback = DlgDoneProc;
  c->dlg_done.dwFlags |=
      TDF_CAN_BE_MINIMIZED | TDF_ENABLE_HYPERLINKS | TDF_SIZE_TO_CONTENT;
  c->dlg_done.pszMainInstruction = L"Done";
  c->dlg_done.cxWidth = c->dlg_done.cxWidth;
  c->dlg_done.pszFooterIcon = TD_SHIELD_ICON;
  c->dlg_done.pszFooter =
      L"GPLv2: <A HREF=\"https://github.com/yzwduck/FontLoaderSub\">"
      L"github.com/yzwduck/FontLoaderSub</A>";
  c->dlg_done.lpCallbackData = (LONG_PTR)c;
  c->dlg_done.pszContent = c->buffer;

  StrDbCreate(&c->alloc, &c->sub_font);
  BsetCreate(&c->alloc, sizeof(uint32_t), &c->bset_file_id);
  BsetCreate(&c->alloc, 32, &c->bset_file_hash);
  if (!CryptAcquireContext(&c->cypt_prov, NULL, NULL, PROV_RSA_AES,
                           CRYPT_VERIFYCONTEXT))
    return 1;

  // command line
  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);
  if (0 && c->argc < 2) {
    // error
    c->error_env = 1;
    return 1;
  }

  return 0;
}

int AppRun(app_ctx_t *c) {
  int nButton = IDCANCEL;
  TaskDialogIndirect(&c->dlg_work, &nButton, NULL, NULL);

  if (WaitForSingleObject(c->thread, 1000 * 15) == WAIT_TIMEOUT) {
    TerminateThread(c->thread, 1);
  }
  AppUnloadFonts(c);
  return 0;
}

#include <ShellScalingApi.h>

BOOL PerMonitorDpiHack() {
  typedef BOOL(WINAPI *
               PFN_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);
  typedef BOOL(WINAPI * PFN_SetProcessDPIAware)(VOID);
  typedef HRESULT(WINAPI * PFN_SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);
  typedef BOOL(WINAPI * PFN_EnablePerMonitorDialogScaling)();
  PFN_SetProcessDpiAwarenessContext pSetProcessDpiAwarenessContext = NULL;
  PFN_EnablePerMonitorDialogScaling pEnablePerMonitorDialogScaling = NULL;
  PFN_SetProcessDPIAware pSetProcessDPIAware = NULL;
  PFN_SetProcessDpiAwareness pSetProcessDpiAwareness = NULL;
  DWORD result = 0;

  HMODULE user32 = GetModuleHandle(L"USER32");
  if (user32 == NULL)
    return FALSE;

  pSetProcessDpiAwarenessContext =
      (PFN_SetProcessDpiAwarenessContext)GetProcAddress(
          user32, "SetProcessDpiAwarenessContext");
  // find a private function, available on RS1, attempt 1
  /*
  pEnablePerMonitorDialogScaling =
      (PFN_EnablePerMonitorDialogScaling)GetProcAddress(
          user32, "EnablePerMonitorDialogScaling");
  */
  if (pEnablePerMonitorDialogScaling == NULL) {
    // attempt 2:
    pEnablePerMonitorDialogScaling =
        (PFN_EnablePerMonitorDialogScaling)GetProcAddress(user32, (LPCSTR)2577);
  }
  pSetProcessDPIAware =
      (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
  pSetProcessDpiAwareness = (PFN_SetProcessDpiAwareness)GetProcAddress(
      user32, "SetProcessDpiAwarenessInternal");

  if (pSetProcessDpiAwarenessContext) {
    // preferred, official API, available since Win10 Creators
    pSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  } else if (pSetProcessDpiAwareness) {
    if (pEnablePerMonitorDialogScaling) {
      // enable per-monitor scaling on Win10RS1+
      result = pSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
      result = pEnablePerMonitorDialogScaling();
    } else {
      result = pSetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);
    }
  } else if (pSetProcessDPIAware) {
    result = pSetProcessDPIAware();
  }

  return 0;
}

void test_version() {
#if 0
  assert(FlVersionCmp(L"version", L"version") == 0);
  assert(FlVersionCmp(L"12345678901234567890", L"12345678901234567890") == 0);
  assert(FlVersionCmp(L"1.900", L"1.199") == 1);
  assert(FlVersionCmp(L"1.9", L"1.19") == -1);
  assert(FlVersionCmp(L"1.09", L"1.9") == 0);
  assert(FlVersionCmp(L"Version 1.004;PS 1.004;hotconv 1.0.82;makeotf.lib2.5.63406 ", L"Version 2.000;hotconv 1.0.107;makeotfexe 2.5.65593") == -1);
#endif
}

app_ctx_t g_ctx;

int WINAPI _tWinMain(HINSTANCE hInstance,
                     HINSTANCE hPrevInstance,
                     LPTSTR lpCmdLine,
                     int nCmdShow) {
  test_version();

  PerMonitorDpiHack();
  // MessageBox(NULL, L"break", L"break", 0);
  app_ctx_t *c = &g_ctx;
  int r = AppInit(c, hInstance);
  if (r == 0) {
    r = AppRun(c);
  } else {
    TaskDialog(NULL, hInstance, kAppTitle, L"Error...", NULL,
               TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
  }
  return r;
}

extern IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
