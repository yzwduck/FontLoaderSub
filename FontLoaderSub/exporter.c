#include "exporter.h"

#include <Shobjidl.h>

#define C_SAVE_RELEASE(obj)          \
  do {                               \
    if ((obj) != NULL) {             \
      (obj)->lpVtbl->Release((obj)); \
      obj = NULL;                    \
    }                                \
  } while (0)

typedef struct LoadedFontEnumCtx {
  const IEnumShellItemsVtbl *lpVtbl;
  FL_AppCtx *app;
  IShellItem *root;
  size_t i;
  ULONG ref;
} LoadedFontEnumCtx;

static HRESULT STDMETHODCALLTYPE
LFEC_QueryInterface(IEnumShellItems *This, REFIID riid, void **out) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  REFIID unk = &IID_IUnknown;
  REFIID esi = &IID_IEnumShellItems;
  if (IsEqualGUID(riid, unk)) {
    *out = (void *)c;
    return S_OK;
  } else if (IsEqualGUID(riid, esi)) {
    c->ref++;
    *out = (void *)c;
    return S_OK;
  } else {
    return E_NOINTERFACE;
  }
}

static ULONG STDMETHODCALLTYPE LFEC_AddRef(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  return ++c->ref;
}

static ULONG STDMETHODCALLTYPE LFEC_Release(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  ULONG ret = --c->ref;
  if (ret == 0) {
    c->root->lpVtbl->Release(c->root);
    c->app->alloc->alloc(c, 0, c->app->alloc->arg);
  }
  return ret;
}

static HRESULT LFEC_NextOne(LoadedFontEnumCtx *c, IShellItem **item) {
  FL_LoaderCtx *fl = &c->app->loader;
  while (c->i != fl->loaded_font.n) {
    FL_FontMatch *data = (FL_FontMatch *)fl->loaded_font.data;
    FL_FontMatch *m = &data[c->i];
    c->i++;
    if (m->filename != NULL && (m->flag & FL_LOAD_DUP) == 0) {
      if (item != NULL) {
        return SHCreateItemFromRelativeName(
            c->root, m->filename, NULL, &IID_IShellItem, (void **)item);
      } else {
        return S_OK;  // simulate create success
      }
    }
  }
  return S_FALSE;  // no more items
}

static HRESULT STDMETHODCALLTYPE LFEC_Next(
    IEnumShellItems *This,
    ULONG celt,
    IShellItem **rgelt,
    ULONG *pceltFetched) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  ULONG got = 0;
  HRESULT hr = S_FALSE;
  for (ULONG i = 0; i != celt; i++) {
    hr = LFEC_NextOne(c, rgelt ? &rgelt[got] : NULL);
    if (hr == S_FALSE) {
      // no more left
      break;
    } else if (FAILED(hr)) {
      // should hide something
      break;
    } else {
      got++;
    }
  }
  if (pceltFetched != NULL) {
    *pceltFetched = got;
  }
  return got > 0 ? S_OK : hr;
}

static HRESULT STDMETHODCALLTYPE LFEC_Skip(IEnumShellItems *This, ULONG celt) {
  return LFEC_Next(This, celt, NULL, NULL);
}

static HRESULT STDMETHODCALLTYPE LFEC_Reset(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  c->i = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
LFEC_Clone(IEnumShellItems *This, IEnumShellItems **ppenum) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  LoadedFontEnumCtx *that = (LoadedFontEnumCtx *)c->app->alloc->alloc(
      NULL, sizeof *c, c->app->alloc->arg);
  if (that == NULL) {
    return E_OUTOFMEMORY;
  }
  that->lpVtbl = c->lpVtbl;
  that->app = c->app;
  that->root = c->root;
  that->i = c->i;
  that->ref = 1;
  c->root->lpVtbl->AddRef(c->root);
  *ppenum = (IEnumShellItems *)that;
  return S_OK;
}

static const IEnumShellItemsVtbl kLFEC_Verb = {
    .QueryInterface = LFEC_QueryInterface,
    .AddRef = LFEC_AddRef,
    .Release = LFEC_Release,
    .Next = LFEC_Next,
    .Skip = LFEC_Skip,
    .Reset = LFEC_Reset,
    .Clone = LFEC_Clone,
};

static HRESULT LFEC_Create(FL_AppCtx *app, IEnumShellItems **ppenum) {
  FL_LoaderCtx *fl = &app->loader;
  WCHAR font_path_hack = 0;
  WCHAR *font_path = (WCHAR *)str_db_get(&fl->font_path, 0);
  if (font_path == NULL)
    return E_FAIL;
  if (font_path[0] == L'\\' && font_path[1] == L'\\' && font_path[2] == L'?' &&
      font_path[3] == L'\\') {
    // case 1: \\?\E:\... -> E:\...
    font_path += 4;
    if (font_path[0] == L'U' && font_path[1] == L'N' && font_path[2] == L'C' &&
        font_path[3] == L'\\') {
      // case 2: \\?\UNC\tsclient\... -> \\tsclient\...
      // hack the first backslash
      font_path += 2;
      font_path_hack = font_path[0];
      font_path[0] = L'\\';
    }
  }
  IShellItem *dir_root = NULL;
  HRESULT hr = SHCreateItemFromParsingName(
      font_path, NULL, &IID_IShellItem, (void **)&dir_root);
  if (font_path_hack) {
    font_path[0] = font_path_hack;
  }
  if (FAILED(hr))
    return hr;

  LoadedFontEnumCtx *that = (LoadedFontEnumCtx *)app->alloc->alloc(
      NULL, sizeof *that, app->alloc->arg);
  if (that == NULL) {
    dir_root->lpVtbl->Release(dir_root);
    return E_OUTOFMEMORY;
  }
  that->lpVtbl = &kLFEC_Verb;
  that->app = app;
  that->root = dir_root;
  that->i = 0;
  that->ref = 1;
  *ppenum = (IEnumShellItems *)that;
  return S_OK;
}

int ExportLoadedFonts(HWND hWnd, FL_AppCtx *c) {
  int succ = 0;
  HRESULT hr;
  IFileDialog *pfd = NULL;
  FILEOPENDIALOGOPTIONS options;
  IShellItem *result = NULL;
  IEnumShellItems *font_enum = NULL;
  LPWSTR path_name = NULL;
  IFileOperation *file_opt = NULL;

  do {
    // prepare "Select folder" dialog
    hr = CoCreateInstance(
        &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
        (void **)&pfd);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->GetOptions(pfd, &options);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->SetOptions(pfd, options | FOS_PICKFOLDERS);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->Show(pfd, hWnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      // cancelled
      succ = 1;
      break;
    } else if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->GetResult(pfd, &result);
    if (FAILED(hr))
      break;
    SIGDN dn = SIGDN_FILESYSPATH;
    hr = result->lpVtbl->GetDisplayName(result, dn, &path_name);
    if (FAILED(hr))
      break;

    // prepare font list and copy
    hr = LFEC_Create(c, &font_enum);
    if (FAILED(hr))
      break;
    hr = CoCreateInstance(
        &CLSID_FileOperation, NULL, CLSCTX_ALL, &IID_IFileOperation,
        (void **)&file_opt);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->SetOwnerWindow(file_opt, hWnd);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->CopyItems(file_opt, (IUnknown *)font_enum, result);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->PerformOperations(file_opt);
    if (FAILED(hr))
      break;

    ShellExecute(NULL, NULL, path_name, NULL, NULL, SW_SHOW);
    succ = 1;
  } while (0);

  C_SAVE_RELEASE(pfd);
  C_SAVE_RELEASE(result);
  C_SAVE_RELEASE(font_enum);
  C_SAVE_RELEASE(file_opt);
  CoTaskMemFree(path_name);
  if (!succ) {
    TaskDialog(
        NULL, c->hInst, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...", NULL,
        TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
  }
  return 0;
}
