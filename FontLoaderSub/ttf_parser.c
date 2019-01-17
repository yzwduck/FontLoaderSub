#include "ttf_parser.h"
#include "util.h"

#define MAKE_TAG(a, b, c, d)                                             \
  ((uint32_t)(((uint8_t)(d) << 24)) | (uint32_t)(((uint8_t)(c) << 16)) | \
   (uint32_t)(((uint8_t)(b) << 8)) | (uint32_t)(((uint8_t)(a))))

typedef enum FONT_TAG {
  FONT_TAG_TTCF = MAKE_TAG('t', 't', 'c', 'f'),
  FONT_TAG_OTTO = MAKE_TAG('O', 'T', 'T', 'O'),
  FONT_TAG_NAME = MAKE_TAG('n', 'a', 'm', 'e')
} FONT_TAG;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint16_t major_ver;
  uint16_t minor_ver;
  uint32_t num_fonts;
} TTC_Header;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint16_t num_tables;
  uint16_t search_range;
  uint16_t entry_selector;
  uint16_t range_shift;
} OTF_Header;

typedef struct {
  union {
    uint32_t tag;
    char tag_chr[4];
  };
  uint32_t checksum;
  uint32_t offset;
  uint32_t length;
} OTF_HeaderRecord;

typedef struct {
  uint16_t format;
  uint16_t count;
  uint16_t offset;
} OTF_NameHeader;

typedef enum OTF_PLATFORM {
  OTF_PLATFORM_UNICODE = 0,
  OTF_PLATFORM_WINDOWS = 3
} OTF_PLATFORM;

static int is_interested_name_id(uint16_t name_id) {
  switch (name_id) {
  case 1:  // Font Family name
  case 4:  // Full font name
    return 1;
  case 0:   // Copyright notice
  case 2:   // Font Subfamily name
  case 3:   // Unique font identifier
  case 5:   // Version string
  case 6:   // PostScript name for the font
  case 7:   // Trademark
  case 8:   // Manufacturer Name
  case 9:   // Designer
  case 10:  // Description
  case 11:  // URL Vendor
  case 12:  // URL Designer
  case 13:  // License Description
  case 14:  // License Info URL
  case 15:  // Reserved
  case 16:  // Typographic Family name
  case 17:  // Typographic Subfamily name
  case 18:  // Compatible Full (Macintosh only)
  case 19:  // Sample text
  case 20:  // PostScript CID find font name
  case 21:  // WWS Family Name
  case 22:  // WWS Subfamily Name
  case 23:  // Light Background Palette
  case 24:  // Dark Background Palette
  case 25:  // Variations PostScript Name Prefix
  default:
    return 0;
  }
}

static int otf_parse_table_name(
    const uint8_t *buffer,
    const uint8_t *eos,
    OTF_NameCallback cb,
    void *arg) {
  OTF_NameHeader *head = (OTF_NameHeader *)buffer;
  if (buffer + sizeof *head > eos)
    return FL_CORRUPTED;

  // extract header
  const uint16_t format = be16(head->format);
  if (format != 0)
    return FL_UNRECOGNIZED;
  const uint16_t count = be16(head->count);
  const uint8_t *str_buffer = buffer + be16(head->offset);
  OTF_NameRecord *records = (OTF_NameRecord *)(head + 1);

  // range check for records
  if (buffer + sizeof *head + count * sizeof records[0] > eos)
    return FL_CORRUPTED;
  // range check for all strings
  for (uint16_t i = 0; i != count; i++) {
    OTF_NameRecord *r = &records[i];
    if (str_buffer + be16(r->offset) + be16(r->length) > eos)
      return FL_CORRUPTED;
  }

  int ret;

  // loop & callback for Version
  for (uint16_t i = 0; i != count; i++) {
    OTF_NameRecord *r = &records[i];
    // filter version string (name_id == 5)
    if (r->name_id == be16(5) && r->platform == be16(OTF_PLATFORM_WINDOWS)) {
      wchar_t *str_be = (wchar_t *)(str_buffer + be16(r->offset));
      // fire callback
      ret = cb(r, str_be, arg);
      if (ret != FL_OK)
        return ret;
    }
  }

  // loop & callback for each record
  for (uint16_t i = 0; i != count; i++) {
    OTF_NameRecord *r = &records[i];
    // filter
    if (r->platform == be16(OTF_PLATFORM_WINDOWS) &&
        is_interested_name_id(be16(r->name_id))) {
      wchar_t *str_be = (wchar_t *)(str_buffer + be16(r->offset));
      // fire callback
      ret = cb(r, str_be, arg);
      if (ret != FL_OK)
        return ret;
    }
  }
  return FL_OK;
}

static int otf_parse_internal(
    const uint8_t *buffer,
    const uint8_t *eos,
    const uint8_t *start,
    OTF_NameCallback cb,
    void *arg) {
  OTF_Header *head = (OTF_Header *)buffer;
  if (buffer + sizeof *head > eos)
    return FL_UNRECOGNIZED;
  if (head->tag != FONT_TAG_OTTO && head->tag != be32(0x00010000))
    return FL_UNRECOGNIZED;

  const uint16_t num_tables = be16(head->num_tables);
  OTF_HeaderRecord *record = (OTF_HeaderRecord *)(head + 1);
  if (buffer + sizeof *head + num_tables * sizeof record[0] > eos)
    return FL_CORRUPTED;

  for (uint16_t i = 0; i != num_tables; i++) {
    if (record[i].tag == FONT_TAG_NAME) {
      const uint8_t *ptr = start + be32(record[i].offset);
      const uint32_t length = be32(record[i].length);
      if (ptr + length > eos)
        return FL_CORRUPTED;
      const int r = otf_parse_table_name(ptr, ptr + length, cb, arg);
      if (r != FL_OK)
        return r;
    }
  }
  return FL_OK;
}

int otf_parse(const uint8_t *buf, size_t size, OTF_NameCallback cb, void *arg) {
  return otf_parse_internal(buf, buf + size, buf, cb, arg);
}

int ttc_parse(const uint8_t *buf, size_t size, OTF_NameCallback cb, void *arg) {
  const uint8_t *eos = buf + size;
  TTC_Header *head = (TTC_Header *)buf;
  if (size < sizeof *head)
    return FL_UNRECOGNIZED;
  if (head->tag != FONT_TAG_TTCF)
    return FL_UNRECOGNIZED;

  const uint32_t num_fonts = be32(head->num_fonts);
  const uint32_t *offset = (uint32_t *)(head + 1);
  if (size < (sizeof *head) + sizeof(offset[0]) * num_fonts)
    return FL_CORRUPTED;

  for (uint32_t i = 0; i != num_fonts; i++) {
    const uint8_t *ptr = buf + be32(offset[i]);
    if (ptr >= eos)
      return FL_CORRUPTED;

    const int r = otf_parse_internal(ptr, eos, buf, cb, arg);
    if (r != FL_OK)
      return r;
  }
  return FL_OK;
}
