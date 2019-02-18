#include "ass_parser.h"

static int null_cb(const wchar_t *font, size_t cch, void *arg) {
  return 0;
}

int test_main() {
  const char data[] = {0x5b, 0x00};
  const wchar_t *wc = (const wchar_t *)data;
  ass_process_data(wc, sizeof data / 2, null_cb, NULL);
  return 1;
}
