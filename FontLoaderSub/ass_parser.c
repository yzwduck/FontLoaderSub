#include "ass_parser.h"
#include "ass_string.h"

typedef enum {
  PST_UNKNOWN = 0,
  // PST_INFO,
  PST_STYLES,
  PST_EVENTS,
  // PST_FONTS
} ASS_ParserState;

typedef enum {
  TRACK_TYPE_UNKNOWN = 0,
  TRACK_TYPE_ASS,
  TRACK_TYPE_SSA
} ASS_TrackType;

typedef struct {
  ASS_Range Text;
} ASS_Event;

typedef struct {
  ASS_ParserState state;
  ASS_TrackType track_type;
  ASS_Range format_string;

  ASS_FontCallback callback;
  void *cb_arg;
} ASS_Track;

static void fire_font_cb(ASS_Track *track, ASS_Range *font) {
  if (track->callback) {
    track->callback(font->begin, font->end - font->begin, track->cb_arg);
  }
}

static int next_tok(ASS_Range *input, ASS_Range *tok) {
  if (input->begin == input->end) {
    return 0;
  }
  *tok = (ASS_Range){.begin = input->begin, .end = input->begin};
  while (tok->end != input->end && tok->end[0] != ',') {
    ++tok->end;
  }
  if (tok->end[0] == ',') {
    input->begin = tok->end + 1;
  } else {
    input->begin = tok->end;
  }
  ass_trim(tok);

  return 1;
}

static int test_tag(
    const wchar_t *p,
    const wchar_t *end,
    const wchar_t *tag,
    size_t len,
    ASS_Range *arg) {
  if (end >= p + len && ass_strncmp(p, tag, len) == 0) {
    *arg = (ASS_Range){.begin = p + len, .end = end};
    return 1;
  }
  return 0;
}

static const wchar_t *
parse_tags(ASS_Track *track, const wchar_t *p, const wchar_t *end, int nested) {
  const wchar_t *q;
  for (; p != end; p = q) {
    while (p != end && *p != '\\')
      ++p;
    if (*p != '\\')
      break;
    ++p;
    if (p != end)
      p = ass_skip_spaces(p, end);

    q = p;
    while (q != end && *q != '(' && *q != '\\')
      ++q;
    if (q == p)
      continue;

    const wchar_t *name_end = q;

    // Split parenthesized arguments
    if (q != end && *q == '(') {
      ++q;
      while (1) {
        if (q != end)
          q = ass_skip_spaces(q, end);
        const wchar_t *r = q;
        while (r != end && *r != ',' && *r != '\\' && *r != ')')
          ++r;

        if (r != end && *r == ',') {
          // push_arg(args, &argc, q, r);
          q = r + 1;
        } else {
          while (r != end && *r != ')')
            ++r;
          // push_arg(args, &argc, q, r);
          q = r;
          if (q != end)
            ++q;
          break;
        }
      }
    }

    ASS_Range arg;
    if (test_tag(p, name_end, L"fn", 2, &arg)) {
      if (ass_strncmp(L"0", arg.begin, arg.end - arg.begin) == 0) {
        // restore?
      } else {
        fire_font_cb(track, &arg);
      }
    }
  }
  return p;
}

static void parse_events(ASS_Track *track, ASS_Event *event) {
  if (event->Text.begin == NULL) {
    return;
  }

  const wchar_t *p = event->Text.begin;
  const wchar_t *ep = event->Text.end;
  const wchar_t *q;

  while ((p = ass_strnchr(p, '{', ep - p)) != NULL &&
         (q = ass_strnchr(p, '}', ep - p)) != NULL) {
    p = parse_tags(track, p, q, 0);
    ++p;
  }
}

static void
process_event_tail(ASS_Track *track, ASS_Range *line, int n_ignored) {
  int i;
  ASS_Range tok[1], tag[1], format[1];
  ASS_Event event = {0};

  for (i = 0; i < n_ignored; i++) {
    next_tok(line, tok);
  }

  *format = track->format_string;
  if (format->begin == format->end) {
    // using fallback
    const int skips = 9;
    for (i = 0; i < skips; i++)
      next_tok(line, tok);
    if (next_tok(line, tok)) {
      tok->end = line->end;
      event.Text = *tok;
    }
  } else {
    while (next_tok(format, tag)) {
      const int r = next_tok(line, tok);
      if (r && tag->end - tag->begin == 4 &&
          ass_strncasecmp(tag->begin, L"text", 4) == 0) {
        // till the end
        tok->end = line->end;
        event.Text = *tok;
        break;
      }
    }
  }
  parse_events(track, &event);
}

static void
process_styles(ASS_Track *track, const wchar_t *begin, const wchar_t *end) {
  ASS_Range line[1], tok[1], tag[1], format[1];
  *line = (ASS_Range){.begin = begin, .end = end};

  *format = track->format_string;
  if (format->begin == format->end) {
    // use default fallback, assuming fontname at column 2
    next_tok(line, tok);
    const int r = next_tok(line, tok);
    if (r) {
      fire_font_cb(track, tok);
    }
  } else {
    // using format string
    while (next_tok(format, tag)) {
      const int r = next_tok(line, tok);
      if (r && tag->end - tag->begin == 8 &&
          ass_strncasecmp(tag->begin, L"fontname", 8) == 0) {
        fire_font_cb(track, tok);
      }
    }
  }
}

static void process_styles_line(
    ASS_Track *track,
    const wchar_t *begin,
    const wchar_t *end) {
  if (!ass_strncmp(begin, L"Format:", 7)) {
    track->format_string = (ASS_Range){.begin = begin + 7, .end = end};
    ass_trim(&track->format_string);
  } else if (!ass_strncmp(begin, L"Style:", 6)) {
    process_styles(track, ass_skip_spaces(begin + 6, end), end);
  }
}

static void process_events_line(
    ASS_Track *track,
    const wchar_t *begin,
    const wchar_t *end) {
  if (!ass_strncmp(begin, L"Format:", 7)) {
    const ASS_Range fmt_str = {.begin = begin + 7, .end = end};
    track->format_string = fmt_str;
    ass_trim(&track->format_string);
  } else if (!ass_strncmp(begin, L"Dialogue:", 9)) {
    ASS_Range range = {.begin = ass_skip_spaces(begin + 9, end), .end = end};
    process_event_tail(track, &range, 0);
  }
}

static void
process_line(ASS_Track *track, const wchar_t *begin, const wchar_t *end) {
  int is_content = 0;

  if (!ass_strncasecmp(begin, L"[v4 styles]", 11)) {
    track->state = PST_STYLES;
    track->track_type = TRACK_TYPE_SSA;
  } else if (!ass_strncasecmp(begin, L"[v4+ styles]", 12)) {
    track->state = PST_STYLES;
    track->track_type = TRACK_TYPE_ASS;
  } else if (!ass_strncasecmp(begin, L"[events]", 8)) {
    track->state = PST_EVENTS;
  } else if (begin[0] == '[') {
    track->state = PST_UNKNOWN;
  } else {
    is_content = 1;
  }

  if (!is_content) {
    track->format_string = (ASS_Range){.begin = NULL, .end = NULL};
  } else {
    switch (track->state) {
    case PST_STYLES:
      process_styles_line(track, begin, end);
      break;
    case PST_EVENTS:
      process_events_line(track, begin, end);
      break;
    default:
      break;
    }
  }
}

void ass_process_data(
    const wchar_t *data,
    size_t cch,
    ASS_FontCallback cb,
    void *arg) {
  ASS_Track track = {.callback = cb, .cb_arg = arg};
  const wchar_t *p = data;
  const wchar_t *eos = data + cch;
  while (p != eos) {
    // skip blank lines
    while (p != eos && ass_is_eol(*p))
      ++p;
    // find end of the line
    const wchar_t *q = p;
    while (q != eos && !ass_is_eol(*q))
      ++q;

    process_line(&track, p, q);
    p = q;
  }
}
