#include "util.h"
#include <Windows.h>
#include <Shlwapi.h>
#include <intrin.h>

#pragma intrinsic(__movsb)
#pragma intrinsic(__stosb)

int FlMemMap(const wchar_t *path, memmap_t *mmap) {
  mmap->map = NULL;
  mmap->data = NULL;
  mmap->size = 0;
  HANDLE h;
  do {
    h = CreateFile(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
      break;
    mmap->map = CreateFileMapping(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (mmap->map == NULL)
      break;
    mmap->data = MapViewOfFile(mmap->map, FILE_MAP_READ, 0, 0, 0);
    if (mmap->data == NULL)
      break;
    DWORD high = 0;
    mmap->size = GetFileSize(h, &high);
    // mmap->size = (high * 0x100000000UL) | (mmap->size);
  } while (0);

  if (mmap->data == NULL) {
    FlMemUnmap(mmap);
  }
  CloseHandle(h);
  return 0;
}

int FlMemUnmap(memmap_t *mmap) {
  UnmapViewOfFile(mmap->data);
  CloseHandle(mmap->map);
  mmap->map = NULL;
  mmap->data = NULL;
  mmap->size = 0;
  return 0;
}

static int FlTestUtf8(const uint8_t *buffer, size_t size) {
  const uint8_t *p, *last;
  int rem = 0;
  for (p = buffer, last = buffer + size; p != last; p++) {
    if (rem) {
      if ((*p & 0xc0) == 0x80) {
        // 10xxxxxx
        --rem;
      } else {
        return 0;
      }
    } else if ((*p & 0x80) == 0) {
      // rem = 0;
    } else if ((*p & 0xd0) == 0xc0) {
      // 110xxxxx
      rem = 1;
    } else if ((*p & 0xf0) == 0xe0) {
      // 1110xxxx
      rem = 2;
    } else if ((*p & 0xf8) == 0xf0) {
      // 11110xxx
      rem = 3;
    } else {
      return 0;
    }
  }
  return rem == 0;
}

static wchar_t *FlTextTryDecode(
    UINT codepage,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *buf = NULL;
  int ok = 0;
  do {
    const int r =
        MultiByteToWideChar(codepage, 0, (const char *)mstr, bytes, NULL, 0);
    *cch = r;
    if (r == 0)
      break;

    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == NULL)
      break;

    const int new_r =
        MultiByteToWideChar(codepage, 0, (const char *)mstr, bytes, buf, r);
    if (new_r == 0 || new_r != r)
      break;
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = NULL;
  }
  return buf;
}

static wchar_t *FlTextDecodeUtf16(
    int big_endian,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  const wchar_t *wstr = (const wchar_t*)mstr;
  wchar_t *buf = NULL;
  int ok = 0;

  do {
    const size_t r = *cch = bytes / 2;
    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == NULL)
      break;

    for (size_t i = 0; i != r; i++) {
      buf[i] = big_endian ? be16(wstr[i]) : wstr[i];
    }
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = NULL;
  }
  return buf;
}

wchar_t *FlTextDecode(
    const uint8_t *buf,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *res = NULL;
  if (bytes < 4)
    return res;

  // detect BOM
  if (buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
    res = FlTextTryDecode(CP_UTF8, buf + 3, bytes - 3, cch, alloc);
  } else if (buf[0] == 0xff && buf[1] == 0xfe) {
    // UTF-16 LE
    res = FlTextDecodeUtf16(0, buf + 2, bytes - 2, cch, alloc);
  } else if (buf[0] == 0xfe && buf[1] == 0xff) {
    // UTF-16 BE
    res = FlTextDecodeUtf16(1, buf + 2, bytes - 2, cch, alloc);
  }

  // detect UTF-8
  if (!res && FlTestUtf8(buf, bytes)) {
    res = FlTextTryDecode(CP_UTF8, buf, bytes, cch, alloc);
  }
  // final resort
  if (!res) {
    res = FlTextTryDecode(CP_ACP, buf, bytes, cch, alloc);
  }
  return res;
}

static int is_digit(wchar_t ch) {
  if (L'0' <= ch && ch <= L'9')
    return ch - L'0';
  else
    return -1;
}

int FlVersionCmp(const wchar_t *a, const wchar_t *b) {
  const wchar_t *ptr_a = a, *ptr_b = b;
  int cmp = 0;

  if (b == NULL)
    return 1;
  if (a == NULL)
    return -1;

  while (*ptr_a && *ptr_b && cmp == 0) {
    if (is_digit(*ptr_a) >= 0 && is_digit(*ptr_b) >= 0) {
      // seek to the end of digits
      const wchar_t *start_a = ptr_a, *start_b = ptr_b;
      while (is_digit(*ptr_a) >= 0)
        ptr_a++;
      while (is_digit(*ptr_b) >= 0)
        ptr_b++;
      // compare from right to left
      const wchar_t *dig_a = ptr_a, *dig_b = ptr_b;
      while (dig_a != start_a && dig_b != start_b) {
        dig_a--, dig_b--;
        cmp = *dig_a - *dig_b;
      }
      // leading zero
      while (dig_a != start_a && dig_a[-1] == L'0') {
        dig_a--;
      }
      while (dig_b != start_b && dig_b[-1] == L'0') {
        dig_b--;
      }
      if (dig_a != start_a) {
        cmp = 1;
      } else if (dig_b != start_b) {
        cmp = -1;
      }
    } else if (*ptr_a != *ptr_b) {
      cmp = *ptr_a - *ptr_b;
    } else {
      ptr_a++, ptr_b++;
    }
  }

  if (cmp == 0) {
    if (*ptr_a)
      cmp = 1;
    else if (*ptr_b)
      cmp = -1;
  }

  return cmp;
}

int FlStrCmpIW(const wchar_t *a, const wchar_t *b) {
  return StrCmpIW(a, b);
}


#include <ShellScalingApi.h>

BOOL PerMonitorDpiHack() {
  typedef BOOL(WINAPI * PFN_SetProcessDpiAwarenessContext)(
      DPI_AWARENESS_CONTEXT value);
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

const TCHAR *ResLoadString(HMODULE hInstance, UINT idText) {
  int res;
  const TCHAR *textptr = NULL;
  res = LoadString(hInstance, idText, (TCHAR *)&textptr, 0);
  if (textptr == NULL) {
    // logA("Failed to load res string");
    textptr = L"";  // failback
  }
  return textptr;
}

void *zmemset(void *dest, int ch, size_t count) {
  __stosb((unsigned char *)dest, (unsigned char)ch, count);
  return dest;
}

void *zmemcpy(void *dest, const void *src, size_t count) {
  __movsb((unsigned char *)dest, (const unsigned char *)src, count);
  return dest;
}
