/* Input decoder: outer-terminal bytes -> semantic events. See input.h. */
#define _GNU_SOURCE
#include "input.h"

#include <ghostty/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct input_parser {
  uint8_t *buf;
  size_t len, cap;
  bool in_paste;
  size_t paste_start; /* offset of paste payload within buf */
};

input_parser_t *input_new(void) {
  input_parser_t *p = calloc(1, sizeof *p);
  p->cap = 4096;
  p->buf = malloc(p->cap);
  return p;
}

void input_free(input_parser_t *p) {
  if (!p) return;
  free(p->buf);
  free(p);
}

static void buf_append(input_parser_t *p, const uint8_t *d, size_t n) {
  if (p->len + n > p->cap) {
    while (p->cap < p->len + n) p->cap *= 2;
    p->buf = realloc(p->buf, p->cap);
  }
  memcpy(p->buf + p->len, d, n);
  p->len += n;
}

static void buf_consume(input_parser_t *p, size_t n) {
  if (n >= p->len) {
    p->len = 0;
    return;
  }
  memmove(p->buf, p->buf + n, p->len - n);
  p->len -= n;
}

/* ---- key tables --------------------------------------------------------- */

/* Printable ASCII -> physical key. Layout-independent codes; the text we send
 * separately is what actually reaches the app for ordinary typing. */
static int ascii_key(uint8_t c) {
  if (c >= 'a' && c <= 'z') return GHOSTTY_KEY_A + (c - 'a');
  if (c >= 'A' && c <= 'Z') return GHOSTTY_KEY_A + (c - 'A');
  if (c >= '0' && c <= '9') return GHOSTTY_KEY_DIGIT_0 + (c - '0');
  switch (c) {
    case ' ': return GHOSTTY_KEY_SPACE;
    case '`': case '~': return GHOSTTY_KEY_BACKQUOTE;
    case '-': case '_': return GHOSTTY_KEY_MINUS;
    case '=': case '+': return GHOSTTY_KEY_EQUAL;
    case '[': case '{': return GHOSTTY_KEY_BRACKET_LEFT;
    case ']': case '}': return GHOSTTY_KEY_BRACKET_RIGHT;
    case '\\': case '|': return GHOSTTY_KEY_BACKSLASH;
    case ';': case ':': return GHOSTTY_KEY_SEMICOLON;
    case '\'': case '"': return GHOSTTY_KEY_QUOTE;
    case ',': case '<': return GHOSTTY_KEY_COMMA;
    case '.': case '>': return GHOSTTY_KEY_PERIOD;
    case '/': case '?': return GHOSTTY_KEY_SLASH;
    case '!': return GHOSTTY_KEY_DIGIT_1;
    case '@': return GHOSTTY_KEY_DIGIT_2;
    case '#': return GHOSTTY_KEY_DIGIT_3;
    case '$': return GHOSTTY_KEY_DIGIT_4;
    case '%': return GHOSTTY_KEY_DIGIT_5;
    case '^': return GHOSTTY_KEY_DIGIT_6;
    case '&': return GHOSTTY_KEY_DIGIT_7;
    case '*': return GHOSTTY_KEY_DIGIT_8;
    case '(': return GHOSTTY_KEY_DIGIT_9;
    case ')': return GHOSTTY_KEY_DIGIT_0;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

/* CSI <n> ~ */
static int tilde_key(int n) {
  switch (n) {
    case 1: case 7: return GHOSTTY_KEY_HOME;
    case 2: return GHOSTTY_KEY_INSERT;
    case 3: return GHOSTTY_KEY_DELETE;
    case 4: case 8: return GHOSTTY_KEY_END;
    case 5: return GHOSTTY_KEY_PAGE_UP;
    case 6: return GHOSTTY_KEY_PAGE_DOWN;
    case 11: return GHOSTTY_KEY_F1;
    case 12: return GHOSTTY_KEY_F2;
    case 13: return GHOSTTY_KEY_F3;
    case 14: return GHOSTTY_KEY_F4;
    case 15: return GHOSTTY_KEY_F5;
    case 17: return GHOSTTY_KEY_F6;
    case 18: return GHOSTTY_KEY_F7;
    case 19: return GHOSTTY_KEY_F8;
    case 20: return GHOSTTY_KEY_F9;
    case 21: return GHOSTTY_KEY_F10;
    case 23: return GHOSTTY_KEY_F11;
    case 24: return GHOSTTY_KEY_F12;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

/* final byte of a CSI/SS3 cursor-ish sequence */
static int final_key(uint8_t f) {
  switch (f) {
    case 'A': return GHOSTTY_KEY_ARROW_UP;
    case 'B': return GHOSTTY_KEY_ARROW_DOWN;
    case 'C': return GHOSTTY_KEY_ARROW_RIGHT;
    case 'D': return GHOSTTY_KEY_ARROW_LEFT;
    case 'E': return GHOSTTY_KEY_NUMPAD_BEGIN;
    case 'H': return GHOSTTY_KEY_HOME;
    case 'F': return GHOSTTY_KEY_END;
    case 'P': return GHOSTTY_KEY_F1;
    case 'Q': return GHOSTTY_KEY_F2;
    case 'R': return GHOSTTY_KEY_F3;
    case 'S': return GHOSTTY_KEY_F4;
    default: return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

/* kitty functional codepoints (CSI u form) */
static int kitty_key(uint32_t cp) {
  switch (cp) {
    case 27: return GHOSTTY_KEY_ESCAPE;
    case 13: return GHOSTTY_KEY_ENTER;
    case 9: return GHOSTTY_KEY_TAB;
    case 127: return GHOSTTY_KEY_BACKSPACE;
    case 57358: return GHOSTTY_KEY_CAPS_LOCK;
    case 57359: return GHOSTTY_KEY_SCROLL_LOCK;
    case 57360: return GHOSTTY_KEY_NUM_LOCK;
    case 57361: return GHOSTTY_KEY_PRINT_SCREEN;
    case 57362: return GHOSTTY_KEY_PAUSE;
    case 57363: return GHOSTTY_KEY_CONTEXT_MENU;
    case 57441: return GHOSTTY_KEY_SHIFT_LEFT;
    case 57442: return GHOSTTY_KEY_CONTROL_LEFT;
    case 57443: return GHOSTTY_KEY_ALT_LEFT;
    case 57444: return GHOSTTY_KEY_META_LEFT;
    case 57447: return GHOSTTY_KEY_SHIFT_RIGHT;
    case 57448: return GHOSTTY_KEY_CONTROL_RIGHT;
    case 57449: return GHOSTTY_KEY_ALT_RIGHT;
    case 57450: return GHOSTTY_KEY_META_RIGHT;
    default:
      if (cp >= 57376 && cp <= 57398) return GHOSTTY_KEY_F13 + (int)(cp - 57376);
      if (cp < 128) return ascii_key((uint8_t)cp);
      return GHOSTTY_KEY_UNIDENTIFIED;
  }
}

/* kitty/xterm modifier param (already decremented) -> our bits */
static uint16_t decode_mods(int m) {
  uint16_t out = 0;
  if (m & 1) out |= MOD_SHIFT;
  if (m & 2) out |= MOD_ALT;
  if (m & 4) out |= MOD_CTRL;
  if (m & 8) out |= MOD_SUPER;
  if (m & 64) out |= MOD_CAPS;
  if (m & 128) out |= MOD_NUM;
  return out;
}

static size_t utf8_len(uint8_t b) {
  if (b < 0x80) return 1;
  if ((b & 0xe0) == 0xc0) return 2;
  if ((b & 0xf0) == 0xe0) return 3;
  if ((b & 0xf8) == 0xf0) return 4;
  return 1; /* invalid lead: consume one byte rather than stall */
}

static uint32_t utf8_decode(const uint8_t *s, size_t n) {
  if (n == 1) return s[0];
  if (n == 2) return (uint32_t)(s[0] & 0x1f) << 6 | (s[1] & 0x3f);
  if (n == 3)
    return (uint32_t)(s[0] & 0x0f) << 12 | (uint32_t)(s[1] & 0x3f) << 6 |
           (s[2] & 0x3f);
  return (uint32_t)(s[0] & 0x07) << 18 | (uint32_t)(s[1] & 0x3f) << 12 |
         (uint32_t)(s[2] & 0x3f) << 6 | (s[3] & 0x3f);
}

/* Capture instead of dispatch; used by the ESC-prefix path. */
static void capture_cb(const input_event_t *ev, void *ud) {
  *(input_event_t *)ud = *ev;
}

static void emit_key(input_cb_t cb, void *ud, int key, uint16_t mods,
                     uint8_t action, const char *text, size_t text_len,
                     uint32_t unshifted) {
  input_event_t ev = {0};
  ev.kind = EV_KEY;
  ev.key = key;
  ev.mods = mods;
  ev.action = action;
  ev.unshifted = unshifted;
  if (text && text_len) {
    if (text_len > sizeof ev.text) text_len = sizeof ev.text;
    memcpy(ev.text, text, text_len);
    ev.text_len = (uint8_t)text_len;
  }
  cb(&ev, ud);
}

/* Parse one event from the front of the buffer.
 * Returns bytes consumed, or 0 if the buffer holds an incomplete sequence. */
static size_t parse_one(input_parser_t *p, input_cb_t cb, void *ud) {
  const uint8_t *b = p->buf;
  size_t n = p->len;
  if (n == 0) return 0;

  /* --- inside a bracketed paste: swallow until the end marker ---------- */
  if (p->in_paste) {
    static const char end[] = "\x1b[201~";
    for (size_t i = 0; i + sizeof end - 1 <= n; i++) {
      if (memcmp(b + i, end, sizeof end - 1) == 0) {
        input_event_t ev = {.kind = EV_PASTE,
                            .paste = (const char *)b,
                            .paste_len = i};
        cb(&ev, ud);
        p->in_paste = false;
        return i + sizeof end - 1;
      }
    }
    return 0; /* keep accumulating */
  }

  uint8_t c = b[0];

  if (c == 0x1b) {
    if (n == 1) return 0; /* bare ESC: resolved by input_timeout() */

    /* --- CSI ---------------------------------------------------------- */
    if (b[1] == '[') {
      size_t i = 2;
      while (i < n && (b[i] < 0x40 || b[i] > 0x7e)) i++;
      if (i >= n) return 0; /* no final byte yet */
      uint8_t final = b[i];
      const uint8_t *params = b + 2;
      size_t plen = i - 2;

      /* bracketed paste start */
      if (final == '~' && plen == 3 && memcmp(params, "200", 3) == 0) {
        p->in_paste = true;
        return i + 1;
      }

      /* focus in/out */
      if (final == 'I' || final == 'O') {
        input_event_t ev = {.kind = EV_FOCUS, .focused = final == 'I'};
        cb(&ev, ud);
        return i + 1;
      }

      /* SGR mouse: CSI < b ; x ; y M|m */
      if (plen >= 1 && params[0] == '<' && (final == 'M' || final == 'm')) {
        int f[3] = {0, 0, 0}, fi = 0;
        for (size_t k = 1; k < plen && fi < 3; k++) {
          if (params[k] == ';') { fi++; continue; }
          if (params[k] >= '0' && params[k] <= '9')
            f[fi] = f[fi] * 10 + (params[k] - '0');
        }
        input_event_t ev = {.kind = EV_MOUSE};
        int btn = f[0];
        ev.mx = (uint16_t)(f[1] > 0 ? f[1] - 1 : 0);
        ev.my = (uint16_t)(f[2] > 0 ? f[2] - 1 : 0);
        if (btn & 4) ev.mods |= MOD_SHIFT;
        if (btn & 8) ev.mods |= MOD_ALT;
        if (btn & 16) ev.mods |= MOD_CTRL;
        bool motion = (btn & 32) != 0;
        int base = btn & 0xc3; /* strip mods+motion, keep high wheel bits */
        if (base >= 64 && base <= 67) {
          ev.button = (uint8_t)(MBTN_FOUR + (base - 64)); /* wheel up/down/l/r */
          ev.maction = MOUSE_PRESS;
        } else {
          switch (base & 3) {
            case 0: ev.button = MBTN_LEFT; break;
            case 1: ev.button = MBTN_MIDDLE; break;
            case 2: ev.button = MBTN_RIGHT; break;
            default: ev.button = MBTN_UNKNOWN; break; /* 3 = release/none */
          }
          ev.maction = motion ? MOUSE_MOTION
                              : (final == 'M' ? MOUSE_PRESS : MOUSE_RELEASE);
        }
        cb(&ev, ud);
        return i + 1;
      }

      /* numeric params, ':' sub-params kept for the kitty forms */
      int nums[8] = {0}, subs[8][3] = {{0}}, ncount = 0, sub_i = 0;
      bool any = false;
      for (size_t k = 0; k < plen && ncount < 8; k++) {
        uint8_t ch = params[k];
        if (ch >= '0' && ch <= '9') {
          any = true;
          if (sub_i == 0) nums[ncount] = nums[ncount] * 10 + (ch - '0');
          else if (sub_i <= 2) subs[ncount][sub_i] = subs[ncount][sub_i] * 10 + (ch - '0');
        } else if (ch == ':') {
          if (sub_i < 2) sub_i++;
        } else if (ch == ';') {
          ncount++;
          sub_i = 0;
        }
      }
      if (any) ncount++;

      int mod_param = ncount >= 2 ? nums[1] : 1;
      uint16_t mods = decode_mods(mod_param > 0 ? mod_param - 1 : 0);
      uint8_t action = KEY_PRESS;
      if (ncount >= 2 && subs[1][1]) {
        int et = subs[1][1];
        action = et == 3 ? KEY_RELEASE : (et == 2 ? KEY_REPEAT : KEY_PRESS);
      }

      /* kitty CSI u */
      if (final == 'u') {
        uint32_t cp = (uint32_t)nums[0];
        uint32_t shifted = (uint32_t)subs[0][1];
        uint32_t base_layout = (uint32_t)subs[0][2];
        int key = kitty_key(base_layout ? base_layout : cp);
        char text[16];
        size_t tlen = 0;
        /* associated text (3rd param) wins; else derive from the codepoint */
        uint32_t out_cp = 0;
        if (ncount >= 3 && nums[2]) out_cp = (uint32_t)nums[2];
        else if (!(mods & (MOD_CTRL | MOD_SUPER)) && cp >= 32 && cp != 127)
          out_cp = (mods & MOD_SHIFT) && shifted ? shifted : cp;
        if (out_cp) {
          if (out_cp < 0x80) text[tlen++] = (char)out_cp;
          else if (out_cp < 0x800) {
            text[tlen++] = (char)(0xc0 | (out_cp >> 6));
            text[tlen++] = (char)(0x80 | (out_cp & 0x3f));
          } else if (out_cp < 0x10000) {
            text[tlen++] = (char)(0xe0 | (out_cp >> 12));
            text[tlen++] = (char)(0x80 | ((out_cp >> 6) & 0x3f));
            text[tlen++] = (char)(0x80 | (out_cp & 0x3f));
          } else {
            text[tlen++] = (char)(0xf0 | (out_cp >> 18));
            text[tlen++] = (char)(0x80 | ((out_cp >> 12) & 0x3f));
            text[tlen++] = (char)(0x80 | ((out_cp >> 6) & 0x3f));
            text[tlen++] = (char)(0x80 | (out_cp & 0x3f));
          }
        }
        emit_key(cb, ud, key, mods, action, text, tlen, cp);
        return i + 1;
      }

      if (final == '~') {
        emit_key(cb, ud, tilde_key(nums[0]), mods, action, NULL, 0, 0);
        return i + 1;
      }

      /* Back-tab. Every terminal sends shift+tab as CSI Z rather than as tab
       * with a modifier param, so it is the one chord whose shift lives in
       * the final byte -- and a picker that walks its list with tab is
       * useless without it. */
      if (final == 'Z') {
        emit_key(cb, ud, GHOSTTY_KEY_TAB, (uint16_t)(mods | MOD_SHIFT), action,
                 NULL, 0, 0);
        return i + 1;
      }

      int key = final_key(final);
      if (key != GHOSTTY_KEY_UNIDENTIFIED) {
        emit_key(cb, ud, key, mods, action, NULL, 0, 0);
        return i + 1;
      }
      return i + 1; /* unrecognised CSI: consume, do not forward */
    }

    /* --- SS3 ---------------------------------------------------------- */
    if (b[1] == 'O') {
      if (n < 3) return 0;
      int key = final_key(b[2]);
      if (key != GHOSTTY_KEY_UNIDENTIFIED)
        emit_key(cb, ud, key, 0, KEY_PRESS, NULL, 0, 0);
      return 3;
    }

    /* --- ESC <byte>: legacy alt ---------------------------------------- */
    /* Parse the tail as its own event and add ALT, so alt+<utf8> and
     * alt+ctrl-x both fall out of the same code path. */
    {
      input_parser_t tail = {.buf = p->buf + 1, .len = n - 1, .cap = n - 1};
      input_event_t captured = {0};
      size_t used = parse_one(&tail, capture_cb, &captured);
      if (used == 0) return 0; /* incomplete: wait for more bytes */
      if (captured.kind == EV_KEY) captured.mods |= MOD_ALT;
      if (captured.kind != EV_NONE) cb(&captured, ud);
      return used + 1;
    }
  }

  /* --- C0 controls ----------------------------------------------------- */
  if (c == '\r') { emit_key(cb, ud, GHOSTTY_KEY_ENTER, 0, KEY_PRESS, "\r", 1, 13); return 1; }
  if (c == '\n') { emit_key(cb, ud, GHOSTTY_KEY_ENTER, 0, KEY_PRESS, "\r", 1, 13); return 1; }
  if (c == '\t') { emit_key(cb, ud, GHOSTTY_KEY_TAB, 0, KEY_PRESS, "\t", 1, 9); return 1; }
  if (c == 0x7f) { emit_key(cb, ud, GHOSTTY_KEY_BACKSPACE, 0, KEY_PRESS, NULL, 0, 127); return 1; }
  if (c == 0x08) { emit_key(cb, ud, GHOSTTY_KEY_BACKSPACE, 0, KEY_PRESS, NULL, 0, 8); return 1; }
  if (c == 0x00) { emit_key(cb, ud, GHOSTTY_KEY_SPACE, MOD_CTRL, KEY_PRESS, NULL, 0, ' '); return 1; }
  if (c < 0x20) {
    int key;
    switch (c) {
      case 0x1c: key = GHOSTTY_KEY_BACKSLASH; break;
      case 0x1d: key = GHOSTTY_KEY_BRACKET_RIGHT; break;
      case 0x1e: key = GHOSTTY_KEY_DIGIT_6; break;
      case 0x1f: key = GHOSTTY_KEY_MINUS; break;
      default: key = GHOSTTY_KEY_A + (c - 1); break;
    }
    uint32_t unshifted = c < 0x1b ? (uint32_t)('a' + c - 1) : 0;
    emit_key(cb, ud, key, MOD_CTRL, KEY_PRESS, NULL, 0, unshifted);
    return 1;
  }

  /* --- text ------------------------------------------------------------ */
  size_t ulen = utf8_len(c);
  if (ulen > n) return 0; /* split UTF-8 across reads */
  uint32_t cp = utf8_decode(b, ulen);
  int key = cp < 128 ? ascii_key((uint8_t)cp) : GHOSTTY_KEY_UNIDENTIFIED;
  uint16_t mods = 0;
  if (cp < 128 && cp >= 'A' && cp <= 'Z') mods |= MOD_SHIFT;
  uint32_t unshifted = cp;
  if (cp >= 'A' && cp <= 'Z') unshifted = cp + 32;
  emit_key(cb, ud, key, mods, KEY_PRESS, (const char *)b, ulen, unshifted);
  return ulen;
}

void input_feed(input_parser_t *p, const uint8_t *data, size_t len,
                input_cb_t cb, void *ud) {
  buf_append(p, data, len);
  for (;;) {
    size_t used = parse_one(p, cb, ud);
    if (used == 0) break;
    buf_consume(p, used);
    if (p->len == 0) break;
  }
}

bool input_pending(const input_parser_t *p) { return p->len > 0; }

void input_timeout(input_parser_t *p, input_cb_t cb, void *ud) {
  if (p->len == 0) return;
  if (p->buf[0] == 0x1b) {
    emit_key(cb, ud, GHOSTTY_KEY_ESCAPE, 0, KEY_PRESS, NULL, 0, 27);
    buf_consume(p, 1);
    /* whatever followed is now re-parsed as ordinary input */
    input_feed(p, NULL, 0, cb, ud);
    return;
  }
  p->len = 0; /* an unterminated non-ESC sequence is garbage; drop it */
}

/* ---- description (tests, logs) ------------------------------------------ */

static const char *key_name(int key) {
  switch (key) {
    case GHOSTTY_KEY_ARROW_UP: return "ARROW_UP";
    case GHOSTTY_KEY_ARROW_DOWN: return "ARROW_DOWN";
    case GHOSTTY_KEY_ARROW_LEFT: return "ARROW_LEFT";
    case GHOSTTY_KEY_ARROW_RIGHT: return "ARROW_RIGHT";
    case GHOSTTY_KEY_ENTER: return "ENTER";
    case GHOSTTY_KEY_TAB: return "TAB";
    case GHOSTTY_KEY_ESCAPE: return "ESCAPE";
    case GHOSTTY_KEY_BACKSPACE: return "BACKSPACE";
    case GHOSTTY_KEY_SPACE: return "SPACE";
    case GHOSTTY_KEY_HOME: return "HOME";
    case GHOSTTY_KEY_END: return "END";
    case GHOSTTY_KEY_INSERT: return "INSERT";
    case GHOSTTY_KEY_DELETE: return "DELETE";
    case GHOSTTY_KEY_PAGE_UP: return "PAGE_UP";
    case GHOSTTY_KEY_PAGE_DOWN: return "PAGE_DOWN";
    case GHOSTTY_KEY_UNIDENTIFIED: return "UNIDENTIFIED";
    default: break;
  }
  if (key >= GHOSTTY_KEY_A && key <= GHOSTTY_KEY_Z) {
    static char buf[2];
    buf[0] = (char)('a' + (key - GHOSTTY_KEY_A));
    buf[1] = 0;
    return buf;
  }
  if (key >= GHOSTTY_KEY_DIGIT_0 && key <= GHOSTTY_KEY_DIGIT_9) {
    static char buf[2];
    buf[0] = (char)('0' + (key - GHOSTTY_KEY_DIGIT_0));
    buf[1] = 0;
    return buf;
  }
  if (key >= GHOSTTY_KEY_F1 && key <= GHOSTTY_KEY_F12) {
    static char buf[4];
    snprintf(buf, sizeof buf, "F%d", key - GHOSTTY_KEY_F1 + 1);
    return buf;
  }
  static char other[16];
  snprintf(other, sizeof other, "key#%d", key);
  return other;
}

void input_event_describe(const input_event_t *ev, char *buf, size_t cap) {
  char mods[64] = "";
  if (ev->mods & MOD_SHIFT) strcat(mods, "S");
  if (ev->mods & MOD_CTRL) strcat(mods, "C");
  if (ev->mods & MOD_ALT) strcat(mods, "A");
  if (ev->mods & MOD_SUPER) strcat(mods, "M");

  switch (ev->kind) {
    case EV_KEY: {
      const char *act = ev->action == KEY_PRESS ? "press"
                        : ev->action == KEY_RELEASE ? "release" : "repeat";
      char text[32] = "";
      if (ev->text_len) {
        snprintf(text, sizeof text, " text=%.*s", (int)ev->text_len, ev->text);
      }
      snprintf(buf, cap, "key %s mods=%s %s%s", key_name(ev->key),
               *mods ? mods : "-", act, text);
      break;
    }
    case EV_MOUSE: {
      const char *act = ev->maction == MOUSE_PRESS ? "press"
                        : ev->maction == MOUSE_RELEASE ? "release" : "motion";
      snprintf(buf, cap, "mouse btn=%u %s at=%u,%u mods=%s", ev->button, act,
               ev->mx, ev->my, *mods ? mods : "-");
      break;
    }
    case EV_PASTE:
      snprintf(buf, cap, "paste len=%zu text=%.*s", ev->paste_len,
               (int)(ev->paste_len > 20 ? 20 : ev->paste_len), ev->paste);
      break;
    case EV_FOCUS:
      snprintf(buf, cap, "focus %s", ev->focused ? "in" : "out");
      break;
    default:
      snprintf(buf, cap, "none");
      break;
  }
}
