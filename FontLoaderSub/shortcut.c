#include "shortcut.h"

#include <Shlwapi.h>
#include <ShlObj.h>
#include <ObjIdl.h>

#include "ass_string.h"

typedef enum {
  SHORTCUT_MODE_QUERY,
  SHORTCUT_MODE_CREATE,
  SHORTCUT_MODE_DELETE
} ShortcutMode;

#define BUTTON_ID_START 1024

typedef void (*ShortcutTogglers)(FL_ShortCtx *c, ShortcutMode mode);

static int ShortcutExplorerDirectory(
    FL_ShortCtx *ctx,
    const WCHAR *key_path,
    ShortcutMode mode);

static void ShortcutRefresh(FL_ShortCtx *ctx);

static HRESULT CALLBACK DlgShortcutProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData);

void ShortcutInit(FL_ShortCtx *c, HINSTANCE hInst, allocator_t *alloc) {
  c->dlg.cbSize = sizeof c->dlg;
  c->dlg.hInstance = hInst;
  c->dlg.pszContent = L"Manage shortcuts";
  c->dlg.pButtons = c->button;
  c->dlg.cButtons = FL_SHORTCUT_MAX;
  c->dlg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  c->dlg.dwFlags |= TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c->dlg.lpCallbackData = (LONG_PTR)c;
  c->dlg.pfCallback = DlgShortcutProc;
  for (int i = 0; i != FL_SHORTCUT_MAX; i++) {
    c->button[i].nButtonID = BUTTON_ID_START + i;
  }
  str_db_init(&c->tmp, alloc, 0, 0);
}

void ShortcutShow(FL_ShortCtx *c, HWND hWnd) {
  c->dlg.hwndParent = hWnd;
  c->dlg.pszWindowTitle = c->dlg_title;
  c->dlg.nDefaultButton = IDCLOSE;
  ShortcutRefresh(c);
  TaskDialogIndirect(&c->dlg, NULL, NULL, NULL);
}

int ShortcutExplorerDirectory(
    FL_ShortCtx *c,
    const WCHAR *key_path,
    ShortcutMode mode) {
  HKEY root = NULL;
  HKEY command = NULL;
  LSTATUS ret;
  int succ = 0;
  str_db_seek(&c->tmp, 0);

  do {
    if (!str_db_push_u16_le(&c->tmp, key_path, 0) ||
        !str_db_push_u16_le(&c->tmp, c->key, 0))
      break;
    const WCHAR *key = str_db_get(&c->tmp, 0);

    if (mode == SHORTCUT_MODE_QUERY) {
      ret = RegOpenKeyEx(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &root);
      if (ret == ERROR_SUCCESS) {
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_DELETE) {
      ret = RegDeleteTree(HKEY_CURRENT_USER, key);
      succ = 1;
    } else if (mode == SHORTCUT_MODE_CREATE) {
      DWORD disposition;
      ret = RegCreateKeyEx(
          HKEY_CURRENT_USER, key, 0, NULL, REG_OPTION_NON_VOLATILE,
          KEY_ALL_ACCESS, NULL, &root, &disposition);
      if (ret != ERROR_SUCCESS)
        break;

      size_t len = ass_strlen(c->explorer_menu_title);
      ret = RegSetValueEx(
          root, NULL, 0, REG_SZ, (const BYTE *)c->explorer_menu_title,
          len * sizeof c->explorer_menu_title[0]);
      if (ret != ERROR_SUCCESS)
        break;

      ret = RegCreateKeyEx(
          root, L"command", 0, NULL, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
          NULL, &command, &disposition);
      if (ret != ERROR_SUCCESS)
        break;

      str_db_seek(&c->tmp, 0);
      if (!str_db_push_u16_le(&c->tmp, L"\"", 0) ||
          !str_db_push_u16_le(&c->tmp, c->path, 0) ||
          !str_db_push_u16_le(&c->tmp, L"\" \"%V\"", 0))
        break;
      const WCHAR *path = str_db_get(&c->tmp, 0);
      ret = RegSetValueEx(
          command, NULL, 0, REG_SZ, (const BYTE *)path,
          str_db_tell(&c->tmp) * sizeof path[0]);
      if (ret != ERROR_SUCCESS)
        break;
      succ = 1;
    }
  } while (0);

  if (command)
    RegCloseKey(command);
  if (root)
    RegCloseKey(root);
  return succ;
}

static int ShortcutExplorerDirectoryBackground(
    FL_ShortCtx *c,
    ShortcutMode mode) {
  return ShortcutExplorerDirectory(
      c, L"Software\\Classes\\Directory\\Background\\shell\\", mode);
}

int ShortcutSendTo(FL_ShortCtx *c, ShortcutMode mode) {
  int succ = 0;
  HRESULT hr;
  PWSTR sendto_path = NULL;
  str_db_seek(&c->tmp, 0);

  do {
    hr = SHGetKnownFolderPath(&FOLDERID_SendTo, 0, NULL, &sendto_path);
    if (FAILED(hr))
      break;
    if (!str_db_push_u16_le(&c->tmp, sendto_path, 0) ||
        !str_db_push_u16_le(&c->tmp, L"\\", 0) ||
        !str_db_push_u16_le(&c->tmp, c->sendto_title, 0) ||
        !str_db_push_u16_le(&c->tmp, L".lnk", 0))
      break;
    const WCHAR *path = str_db_get(&c->tmp, 0);
    if (mode == SHORTCUT_MODE_QUERY) {
      HANDLE h = CreateFile(
          path, 0, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
          NULL);
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_DELETE) {
      HANDLE h = CreateFile(
          path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_CREATE) {
      IShellLink *psl;
      hr = CoCreateInstance(
          &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLink,
          (void **)&psl);
      if (SUCCEEDED(hr)) {
        // set shortcut target
        hr = psl->lpVtbl->SetPath(psl, c->path);
        if (SUCCEEDED(hr)) {
          IPersistFile *ppf;
          hr = psl->lpVtbl->QueryInterface(
              psl, &IID_IPersistFile, (void **)&ppf);
          if (SUCCEEDED(hr)) {
            hr = ppf->lpVtbl->Save(ppf, path, TRUE);
            if (SUCCEEDED(hr)) {
              succ = 1;
            }
            ppf->lpVtbl->Release(ppf);
          }
        }
        psl->lpVtbl->Release(psl);
      }
    }
  } while (0);
  if (sendto_path)
    CoTaskMemFree(sendto_path);
  return succ;
}

static void ShortcutRefresh(FL_ShortCtx *c) {
  c->setup[FL_SHORTCUT_CONTEXT] =
      ShortcutExplorerDirectoryBackground(c, SHORTCUT_MODE_QUERY);
  c->button[FL_SHORTCUT_CONTEXT].pszButtonText =
      c->setup[FL_SHORTCUT_CONTEXT] ? L"Remove from directory background"
                                    : L"Add to directory background";
  c->setup[FL_SHORTCUT_SENDTO] = ShortcutSendTo(c, SHORTCUT_MODE_QUERY);
  c->button[FL_SHORTCUT_SENDTO].pszButtonText =
      c->setup[FL_SHORTCUT_SENDTO] ? L"Remove from SendTo" : L"Add to SendTo";
}

static const ShortcutTogglers kShortcutToggler[FL_SHORTCUT_MAX] = {
    [FL_SHORTCUT_SENDTO] = ShortcutSendTo,
    [FL_SHORTCUT_CONTEXT] = ShortcutExplorerDirectoryBackground};

static HRESULT CALLBACK DlgShortcutProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  FL_ShortCtx *c = (FL_ShortCtx *)dwRefData;
  switch (uNotification) {
  case TDN_BUTTON_CLICKED: {
    if (BUTTON_ID_START <= wParam &&
        wParam < BUTTON_ID_START + FL_SHORTCUT_MAX) {
      int id = wParam - BUTTON_ID_START;
      kShortcutToggler[id](
          c, c->setup[id] ? SHORTCUT_MODE_DELETE : SHORTCUT_MODE_CREATE);
      ShortcutRefresh(c);
      c->dlg.nDefaultButton = wParam;
      SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg);
      return S_FALSE;
    }
  }
  }
  // return S_FALSE; // not to close
  return S_OK;  // otherwise
}
