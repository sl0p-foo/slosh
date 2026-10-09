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
static void dispatch_5577(const char *body, size_t len, osc5577_fn cb,
                          void *ud) {
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

/* ---------------------------------------------------------------- OSC 7501 */

const char *osc7501_state_name(ps_state_t s) {
  switch (s) {
  case PS_IDLE: return "idle";
  case PS_WORKING: return "working";
  case PS_DONE: return "done";
  case PS_BLOCKED: return "blocked";
  case PS_ERROR: return "error";
  default: return "";
  }
}

const char *osc7501_kind_name(ps_kind_t k) {
  switch (k) {
  case PS_KIND_PERMISSION: return "permission";
  case PS_KIND_QUESTION: return "question";
  case PS_KIND_AUTH: return "auth";
  default: return "";
  }
}

bool osc7501_is_descendant(const char *id, const char *parent) {
  if (!*parent) return *id != 0; /* the root is above everything but itself */
  size_t n = strlen(parent);
  return strncmp(id, parent, n) == 0 && id[n] == '/';
}

/* value := [A-Za-z0-9_.,+/=-]* -- no `:` and no `;`, which is why nothing in
 * this protocol needs escaping, and base64's `+/=` all fit. */
static bool value_byte(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == ',' ||
         c == '+' || c == '/' || c == '=' || c == '-';
}

static bool app_byte(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '+' || c == '-';
}

/* An id segment allows the same bytes an app name does. Named separately
 * because the spec states them as two grammars, and a later revision is free
 * to widen one without the other. */
static bool id_byte(char c) { return app_byte(c); }

static bool valid_id(const char *id, size_t len) {
  if (len == 0) return true; /* absent: the root record */
  if (len > 128) return false;
  size_t seg = 0, depth = 1;
  for (size_t i = 0; i < len; i++) {
    if (id[i] == '/') {
      if (seg == 0) return false; /* empty segment: "a//b", "/a", "a/" */
      seg = 0;
      if (++depth > 8) return false;
      continue;
    }
    if (!id_byte(id[i])) return false;
    if (++seg > 32) return false;
  }
  return seg != 0;
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* Standard base64, padding optional (the spec allows either). Writes the
 * decoded bytes to `out`, which must hold `cap`; returns the decoded length,
 * or -1 for input that is not base64 or does not fit. */
static long b64decode(const char *in, size_t len, uint8_t *out, size_t cap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (in[i] == '=') { /* padding, and nothing but padding after it */
      for (size_t j = i; j < len; j++)
        if (in[j] != '=') return -1;
      break;
    }
    int v = b64val(in[i]);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return -1;
      out[n++] = (uint8_t)(acc >> bits);
    }
  }
  /* Leftover bits must be zero padding, not a truncated character: a stray
   * 6 bits means the sender's encoder, or the sender, is lying. */
  if (bits >= 6) return -1;
  if (bits && (acc & ((1u << bits) - 1))) return -1;
  return (long)n;
}

/* Decoded text must be well-formed UTF-8 with no control characters: C0, DEL
 * and C1 (kitty's "escape code safe UTF-8", which is what the spec means and
 * what libghostty-vt checks with `encoding.isSafeUtf8`).
 *
 * Both halves matter and for different reasons. The control characters are the
 * one thing this protocol is careful not to hand a program -- a way to draw
 * outside its own pane -- and checking bytes rather than codepoints would miss
 * a C1 written as `C2 9B`. Well-formedness matters because this text leaves
 * the terminal: it is drawn in a frame, and it is handed to the control API as
 * JSON, where a malformed sequence makes somebody else's parser the one
 * dealing with our untrusted input. */
static bool safe_utf8(const uint8_t *s, size_t len) {
  for (size_t i = 0; i < len;) {
    uint8_t c = s[i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c & 0xe0) == 0xc0) {
      cp = c & 0x1fu;
      n = 2;
    } else if ((c & 0xf0) == 0xe0) {
      cp = c & 0x0fu;
      n = 3;
    } else if ((c & 0xf8) == 0xf0) {
      cp = c & 0x07u;
      n = 4;
    } else {
      return false; /* a continuation byte or an invalid lead */
    }
    if (i + n > len) return false;
    for (size_t k = 1; k < n; k++) {
      if ((s[i + k] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (s[i + k] & 0x3fu);
    }
    /* Overlongs, surrogates and anything past the last plane: all of them
     * encode a codepoint some decoder somewhere will read differently. */
    static const uint32_t min[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < min[n] || cp > 0x10ffff) return false;
    if (cp >= 0xd800 && cp <= 0xdfff) return false;
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f)) return false;
    i += n;
  }
  return true;
}

/* Copy up to cap-1 bytes without splitting a UTF-8 sequence: a truncated
 * status is a shortened one, not a broken one.
 *
 * The boundary walk only runs when there is something to cut off. Doing it
 * unconditionally reads src[len] -- one past the decoded bytes, which is
 * uninitialised stack -- and a byte that happened to look like a continuation
 * there cost every message its last character. */
static void keep_text(char *dst, size_t cap, const uint8_t *src, size_t len) {
  size_t n = len;
  if (n > cap - 1) {
    n = cap - 1;
    while (n > 0 && (src[n] & 0xc0) == 0x80) n--; /* back to a lead byte */
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

static void trim(const char **p, const char **end) {
  while (*p < *end && (**p == ' ' || **p == '\t')) (*p)++;
  while (*end > *p && ((*end)[-1] == ' ' || (*end)[-1] == '\t')) (*end)--;
}

static bool keyis(const char *k, size_t klen, const char *name) {
  return klen == strlen(name) && memcmp(k, name, klen) == 0;
}

/* One key=value pair into the report under construction. False discards the
 * whole report, which is the spec's answer to anything that breaks a limit --
 * so the check happens here, before a record has been touched. */
static bool apply(osc7501_t *r, const char *k, size_t klen, const char *v,
                  size_t vlen) {
  if (keyis(k, klen, "state")) {
    if (keyis(v, vlen, "idle"))
      r->state = PS_IDLE;
    else if (keyis(v, vlen, "working"))
      r->state = PS_WORKING;
    else if (keyis(v, vlen, "done"))
      r->state = PS_DONE;
    else if (keyis(v, vlen, "blocked"))
      r->state = PS_BLOCKED;
    else if (keyis(v, vlen, "error"))
      r->state = PS_ERROR;
    else if (keyis(v, vlen, "clear"))
      r->state = PS_CLEAR;
    /* An unrecognised state discards the report rather than degrading to
     * idle, so a state added in a later revision cannot make an older build
     * announce the opposite of what is happening. */
    else
      return false;
    return true;
  }
  if (keyis(k, klen, "id")) {
    if (!valid_id(v, vlen)) return false; /* never falls back to the root */
    memcpy(r->id, v, vlen);
    r->id[vlen] = 0;
    return true;
  }
  if (keyis(k, klen, "kind")) {
    if (keyis(v, vlen, "permission"))
      r->kind = PS_KIND_PERMISSION;
    else if (keyis(v, vlen, "question"))
      r->kind = PS_KIND_QUESTION;
    else if (keyis(v, vlen, "auth"))
      r->kind = PS_KIND_AUTH;
    else
      r->kind = PS_KIND_NONE; /* unrecognised: absent */
    return true;
  }
  if (keyis(k, klen, "progress")) {
    r->progress = -1; /* anything but an integer 0..100 is "unknown" */
    if (!vlen || vlen > 3) return true;
    int n = 0;
    for (size_t i = 0; i < vlen; i++) {
      if (v[i] < '0' || v[i] > '9') return true;
      n = n * 10 + (v[i] - '0');
    }
    if (n <= 100) r->progress = n;
    return true;
  }
  if (keyis(k, klen, "app")) {
    if (vlen > 32) return false; /* a limit, so: discard */
    for (size_t i = 0; i < vlen; i++)
      if (!app_byte(v[i])) {
        r->app[0] = 0; /* outside the character set: absent */
        return true;
      }
    memcpy(r->app, v, vlen);
    r->app[vlen] = 0;
    return true;
  }
  bool msg = keyis(k, klen, "msg"), title = keyis(k, klen, "title");
  if (msg || title) {
    /* Encoded size first, so a hostile report cannot make us decode megabytes
     * to find out it was too big. */
    if (vlen > (msg ? 2732u : 256u)) return false;
    uint8_t dec[2049];
    long n = b64decode(v, vlen, dec, msg ? 2048 : 192);
    if (n < 0) return false;
    if (!safe_utf8(dec, (size_t)n)) return false;
    keep_text(msg ? r->msg : r->title, OSC7501_KEEP, dec, (size_t)n);
    return true;
  }
  return true; /* an unknown key is ignored: that is how this is extended */
}

bool osc7501_parse(const char *pairs, size_t len, osc7501_t *out) {
  if (len > OSC7501_MAX_BODY) return false;
  osc7501_t r = {.progress = -1};
  const char *p = pairs, *end = pairs + len;
  bool any_state = false;

  while (p < end) {
    const char *colon = memchr(p, ':', (size_t)(end - p));
    const char *fend = colon ? colon : end;
    const char *fp = p;
    p = colon ? colon + 1 : end;

    trim(&fp, &fend);
    const char *eq = memchr(fp, '=', (size_t)(fend - fp));
    if (!eq) continue; /* malformed pair: skipped, the rest still counts */
    const char *ke = eq, *vs = eq + 1;
    trim(&fp, &ke);
    trim(&vs, &fend);
    size_t klen = (size_t)(ke - fp), vlen = (size_t)(fend - vs);

    if (!klen) continue;
    if (klen > 16) return false; /* a limit, not a malformed pair */
    bool ok = true;
    for (size_t i = 0; i < klen; i++)
      if (fp[i] < 'a' || fp[i] > 'z') ok = false;
    for (size_t i = 0; i < vlen; i++)
      if (!value_byte(vs[i])) ok = false;
    if (!ok) continue;

    if (!apply(&r, fp, klen, vs, vlen)) return false;
    if (keyis(fp, klen, "state")) any_state = true;
  }

  if (!any_state) return false; /* state is the one required key */
  /* kind and progress exist only where the spec says they mean anything. */
  if (r.state != PS_BLOCKED) r.kind = PS_KIND_NONE;
  if (r.state != PS_WORKING && r.state != PS_BLOCKED) r.progress = -1;
  *out = r;
  return true;
}

static void dispatch_7501(const char *body, size_t len, bool bel,
                          const osc_scan_cbs_t *cbs, void *ud) {
  const char *pairs = body + 5;
  size_t plen = len - 5;

  /* Feature detection: `OSC 7501 ; ?`, answered with the same body -- and,
   * because the program is matching on bytes it chose, the same terminator it
   * asked with. (lib-vt does the same upstream.) */
  if (plen == 1 && *pairs == '?') {
    if (cbs->status_query) cbs->status_query(bel, ud);
    return;
  }
  osc7501_t rep;
  if (!cbs->status || !osc7501_parse(pairs, plen, &rep)) return;
  cbs->status(&rep, ud);
}

static void dispatch(const char *body, size_t len, bool bel,
                     const osc_scan_cbs_t *cbs, void *ud) {
  if (len >= 5 && memcmp(body, "5577;", 5) == 0) {
    if (cbs->verb) dispatch_5577(body, len, cbs->verb, ud);
    return;
  }
  if (len >= 5 && memcmp(body, "7501;", 5) == 0) {
    dispatch_7501(body, len, bel, cbs, ud);
    return;
  }
  /* OSC 133 ; A: a shell prompt begins, with or without the parameters some
   * shells add (`133;A;aid=1`). Only this one subcommand, and only as a
   * lifetime signal -- we are not implementing shell integration here. */
  if (len >= 5 && memcmp(body, "133;A", 5) == 0 && (len == 5 || body[5] == ';'))
    if (cbs->prompt) cbs->prompt(ud);
}

void osc_scan_feed(osc_scan_t *s, const uint8_t *data, size_t len,
                   const osc_scan_cbs_t *cbs, void *ud) {
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
      } else if (c == 'c') {
        /* RIS. 7501 says a full reset removes every record (a *soft* reset,
         * CSI ! p, does not) -- so it is spotted here rather than asked of
         * lib-vt, which reports us no such thing. */
        if (cbs->reset) cbs->reset(ud);
        s->state = OS_GROUND;
      } else {
        s->state = c == 0x1b ? OS_ESC : OS_GROUND;
      }
      break;

    case OS_BODY:
      if (c == 0x07) { /* BEL terminator, as accepted for OSC 0/2 */
        if (!s->overflow) dispatch(s->buf, s->len, true, cbs, ud);
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
        if (!s->overflow) dispatch(s->buf, s->len, false, cbs, ud);
        s->state = OS_GROUND;
      } else if (c == ']') {
        s->state = OS_BODY; /* a new OSC started inside one: restart */
        s->len = 0;
        s->overflow = false;
      } else if (c == 'c') {
        /* `ESC c` where an ST was expected: the OSC is abandoned and this is
         * a RIS, which is how lib-vt reads it too. Dropping it here instead
         * would leave our records alive across a reset that happened. */
        if (cbs->reset) cbs->reset(ud);
        s->state = OS_GROUND;
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
