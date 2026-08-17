#define _GNU_SOURCE
#include "osc5577.h"

#include <stdlib.h>
#include <string.h>

void osc_scan_reset(osc_scan_t *s) {
  s->state = OS_GROUND;
  s->len = 0;
  s->overflow = false;
}

/* Split "<version>;<verb>;<rest...>" and dispatch. The payload keeps every
 * separator after the verb, so `status;a;b;c` is the text "a;b;c". */
static void dispatch(const char *body, size_t len, osc5577_fn cb, void *ud) {
  if (len < 5 || memcmp(body, "5577;", 5) != 0) return;
  const char *p = body + 5;
  const char *end = body + len;

  const char *semi = memchr(p, ';', (size_t)(end - p));
  if (!semi) return;
  /* An unknown version is ignored *entirely*, so a future version cannot be
   * half-interpreted by an older build. */
  if (semi - p != 1 || *p != '1') return;
  p = semi + 1;

  semi = memchr(p, ';', (size_t)(end - p));
  size_t verb_len = semi ? (size_t)(semi - p) : (size_t)(end - p);
  char verb[32];
  if (verb_len >= sizeof verb) return;
  memcpy(verb, p, verb_len);
  verb[verb_len] = 0;

  const char *payload = semi ? semi + 1 : end;
  size_t plen = (size_t)(end - payload);
  char *buf = malloc(plen + 1);
  memcpy(buf, payload, plen);
  buf[plen] = 0;
  cb(verb, buf, ud);
  free(buf);
}

void osc_scan_feed(osc_scan_t *s, const uint8_t *data, size_t len,
                   osc5577_fn cb, void *ud) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    switch (s->state) {
    case OS_GROUND:
      if (c == 0x1b) s->state = OS_ESC;
      break;

    case OS_ESC:
      if (c == ']') {
        s->state = OS_BODY;
        s->len = 0;
        s->overflow = false;
      } else {
        s->state = c == 0x1b ? OS_ESC : OS_GROUND;
      }
      break;

    case OS_BODY:
      if (c == 0x07) { /* BEL terminator, as accepted for OSC 0/2 */
        if (!s->overflow) dispatch(s->buf, s->len, cb, ud);
        s->state = OS_GROUND;
      } else if (c == 0x1b) {
        s->state = OS_BODY_ESC;
      } else if (c < 0x20) {
        s->state = OS_GROUND; /* a control byte ends a malformed OSC */
      } else if (s->len < sizeof s->buf) {
        s->buf[s->len++] = (char)c;
      } else {
        s->overflow = true; /* keep scanning to the terminator, then drop */
      }
      break;

    case OS_BODY_ESC:
      if (c == '\\') { /* ST */
        if (!s->overflow) dispatch(s->buf, s->len, cb, ud);
        s->state = OS_GROUND;
      } else if (c == ']') {
        s->state = OS_BODY; /* a new OSC started inside one: restart */
        s->len = 0;
        s->overflow = false;
      } else {
        s->state = OS_GROUND;
      }
      break;
    }
  }
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t osc5577_unescape(const char *in, size_t in_len, char *out, size_t cap) {
  size_t n = 0;
  for (size_t i = 0; i < in_len && n + 1 < cap; i++) {
    if (in[i] == '%' && i + 2 < in_len) {
      int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out[n++] = (char)(hi * 16 + lo);
        i += 2;
        continue;
      }
      /* An invalid escape is left literal rather than dropping text. */
    }
    out[n++] = in[i];
  }
  out[n] = 0;
  return n;
}

bool osc5577_valid_id(const char *id) {
  size_t n = strlen(id);
  if (n == 0 || n > 32) return false;
  for (size_t i = 0; i < n; i++) {
    char c = id[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}
