#pragma once

#include <Windows.h>
#include <CommCtrl.h>

#include "cstl.h"

enum { FL_SHORTCUT_SENDTO = 0, FL_SHORTCUT_CONTEXT, FL_SHORTCUT_MAX };

typedef struct {
  const WCHAR *key;
  const WCHAR *dlg_title;
  const WCHAR *sendto_title;
  const WCHAR *explorer_menu_title;
  const WCHAR *path;
  str_db_t tmp;
  TASKDIALOGCONFIG dlg;
  TASKDIALOG_BUTTON button[FL_SHORTCUT_MAX];
  int setup[FL_SHORTCUT_MAX];
} FL_ShortCtx;

void ShortcutInit(FL_ShortCtx *ctx, HINSTANCE hInst, allocator_t *alloc);

void ShortcutShow(FL_ShortCtx *ctx, HWND hWnd);
