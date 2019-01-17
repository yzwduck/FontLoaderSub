#pragma once

#include <stdint.h>

typedef struct {
  uint16_t platform;
  uint16_t encoding;
  uint16_t lang_id;
  uint16_t name_id;
  uint16_t length;  // bytes for string
  uint16_t offset;  // to string, from otf_name_header::offset
} OTF_NameRecord;

typedef int (
    *OTF_NameCallback)(OTF_NameRecord *r, const wchar_t *str, void *arg);

int otf_parse(const uint8_t *buf, size_t size, OTF_NameCallback cb, void *arg);

int ttc_parse(const uint8_t *buf, size_t size, OTF_NameCallback cb, void *arg);
