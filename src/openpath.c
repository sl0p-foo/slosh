/* Clicking a path open: finding it in text, finding it on disk, and spelling the
 * editor command. See include/openpath.h for the contract. */
#define _GNU_SOURCE
#include "openpath.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- finding it in text ------------------------------------------------- */

/* What ends a path in running text. Whitespace, quotes and brackets are the
 * obvious ones -- `"src/a.c"`, `(see src/a.c)`, `[src/a.c]` -- and `=` is for
 * `--output=build/x` and `CC=...`. Colons are *not*: they carry the line
 * number. Bytes >= 0x80 are always path: a name in any script is a name. */
static bool delim(unsigned char c) {
  if (c <= ' ' || c == 0x7f) return true;
  return strchr("\"'`<>|()[]{};=", c) != NULL;
}

static bool all_digits(const char *s, size_t n) {
  if (!n || n > 9) return false; /* a line number, not a hash */
  for (size_t i = 0; i < n; i++)
    if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

/* Peel one `:N` off the end of tok[0..*n). */
static bool peel_number(const char *tok, size_t *n, long *out) {
  size_t i = *n;
  while (i && isdigit((unsigned char)tok[i - 1])) i--;
  if (i == *n || i == 0 || tok[i - 1] != ':') return false;
  if (!all_digits(tok + i, *n - i)) return false;
  *out = strtol(tok + i, NULL, 10);
  *n = i - 1;
  return true;
}

/* `foo.c(12)` / `foo.c(12,5)` (MSVC), or `"foo.py", line 12` (Python), in the
 * text right after the token. Only consulted when the token itself carried no
 * line. */
static void trailing_line(const char *t, size_t len, size_t e,
                          openpath_ref_t *out) {
  if (e < len && t[e] == '(') {
    size_t i = e + 1, j = i;
    while (j < len && isdigit((unsigned char)t[j])) j++;
    if (!all_digits(t + i, j - i)) return;
    long line = strtol(t + i, NULL, 10), col = 0;
    if (j < len && t[j] == ',') {
      size_t k = j + 1;
      while (k < len && isdigit((unsigned char)t[k])) k++;
      if (!all_digits(t + j + 1, k - j - 1)) return;
      col = strtol(t + j + 1, NULL, 10);
      j = k;
    }
    if (j < len && t[j] == ')') {
      out->line = line;
      out->col = col;
    }
    return;
  }
  static const char py[] = ", line ";
  if (e < len && (t[e] == '"' || t[e] == '\'') &&
      len - e - 1 >= sizeof py - 1 &&
      memcmp(t + e + 1, py, sizeof py - 1) == 0) {
    size_t i = e + 1 + sizeof py - 1, j = i;
    while (j < len && isdigit((unsigned char)t[j])) j++;
    if (all_digits(t + i, j - i)) out->line = strtol(t + i, NULL, 10);
  }
}

bool openpath_scan(const char *text, size_t len, size_t at,
                   openpath_ref_t *out) {
  if (!text || !out || at >= len) return false;
  memset(out, 0, sizeof *out);
  if (delim((unsigned char)text[at])) return false;

  size_t s = at, e = at;
  while (s > 0 && !delim((unsigned char)text[s - 1])) s--;
  while (e < len && !delim((unsigned char)text[e])) e++;

  const char *tok = text + s;
  size_t n = e - s;

  /* A URL is somebody else's to open, except the one kind that is a path. */
  if (n >= 7 && strncasecmp(tok, "file://", 7) == 0) {
    char uri[OPENPATH_MAX];
    if (n >= sizeof uri) return false;
    memcpy(uri, tok, n);
    uri[n] = 0;
    if (!openpath_from_uri(uri, n, NULL, out)) return false;
    out->start = s;
    out->end = e;
    return true;
  }
  for (size_t i = 0; i + 2 < n; i++)
    if (tok[i] == ':' && tok[i + 1] == '/' && tok[i + 2] == '/') return false;

  /* Sentence punctuation is not part of the name: `see src/a.c.` or
   * `src/a.c:12:5: error` (that trailing colon). A name made only of dots
   * (`.`, `..`) is a directory, and keeps them. */
  while (n && strchr(".,:;!?", tok[n - 1])) {
    bool dots = true;
    for (size_t i = 0; i < n; i++)
      if (tok[i] != '.') dots = false;
    if (dots) break;
    n--;
  }

  size_t matched = n; /* the span a hover marks keeps its `:12:5` */
  long a = 0, b = 0;
  if (peel_number(tok, &n, &a)) {
    if (peel_number(tok, &n, &b)) {
      out->line = b;
      out->col = a;
    } else {
      out->line = a;
    }
  }
  if (!n || n >= sizeof out->path) return false;
  memcpy(out->path, tok, n);
  out->path[n] = 0;
  out->start = s;
  out->end = s + matched;

  if (!out->line) trailing_line(text, len, e, out);
  return true;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = tolower(c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool openpath_from_uri(const char *uri, size_t len, const char *hostname,
                       openpath_ref_t *out) {
  if (!uri || !out || len < 7 || strncasecmp(uri, "file://", 7) != 0)
    return false;
  memset(out, 0, sizeof *out);
  const char *p = uri + 7, *end = uri + len;

  /* The authority: empty (`file:///x`), localhost, or this machine. */
  const char *slash = memchr(p, '/', (size_t)(end - p));
  if (!slash) return false;
  size_t hl = (size_t)(slash - p);
  if (hl && !(hl == 9 && strncasecmp(p, "localhost", 9) == 0) &&
      !(hostname && strlen(hostname) == hl &&
        strncasecmp(p, hostname, hl) == 0))
    return false;

  /* A fragment can carry the line: `#L12` (forges) or `#12`. A query never
   * belongs to a file name. */
  const char *stop = slash;
  while (stop < end && *stop != '#' && *stop != '?') stop++;
  if (stop < end && *stop == '#') {
    const char *f = stop + 1;
    if (f < end && (*f == 'L' || *f == 'l')) f++;
    if (all_digits(f, (size_t)(end - f))) out->line = strtol(f, NULL, 10);
  }

  size_t o = 0;
  for (const char *q = slash; q < stop; q++) {
    char c = *q;
    if (c == '%' && stop - q >= 3 && hexval(q[1]) >= 0 && hexval(q[2]) >= 0) {
      c = (char)(hexval(q[1]) * 16 + hexval(q[2]));
      q += 2;
    }
    if (!c || o + 1 >= sizeof out->path) return false;
    out->path[o++] = c;
  }
  out->path[o] = 0;
  return o > 0;
}

/* ---- finding it on disk ------------------------------------------------- */

static bool exists(const char *p) {
  struct stat st;
  return stat(p, &st) == 0;
}

static bool absolute(const char *p) {
#ifdef _WIN32
  if (isalpha((unsigned char)p[0]) && p[1] == ':') return true;
  if (p[0] == '\\') return true;
#endif
  return p[0] == '/';
}

static bool try_one(const char *path, const char *const *dirs, size_t ndirs,
                    char *out, size_t cap) {
  char buf[OPENPATH_MAX * 2];
  if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    snprintf(buf, sizeof buf, "%s%s", home, path + 1);
    if (!exists(buf)) return false;
    snprintf(out, cap, "%s", buf);
    return true;
  }
  if (absolute(path)) {
    if (!exists(path)) return false;
    snprintf(out, cap, "%s", path);
    return true;
  }
  while (path[0] == '.' && path[1] == '/') path += 2;
  for (size_t i = 0; i < ndirs; i++) {
    if (!dirs[i] || !*dirs[i]) continue;
    size_t dl = strlen(dirs[i]);
    bool sep = dl && dirs[i][dl - 1] == '/';
    snprintf(buf, sizeof buf, "%s%s%s", dirs[i], sep ? "" : "/",
             *path ? path : ".");
    if (exists(buf)) {
      snprintf(out, cap, "%s", buf);
      return true;
    }
  }
  return false;
}

bool openpath_resolve(const char *path, const char *const *dirs, size_t ndirs,
                      char *out, size_t cap) {
  if (!path || !*path || !out || !cap) return false;
  if (try_one(path, dirs, ndirs, out, cap)) return true;
  /* `git diff` prints `a/src/x.c` and `b/src/x.c`, relative to the root. */
  if ((path[0] == 'a' || path[0] == 'b') && path[1] == '/' && path[2])
    return try_one(path + 2, dirs, ndirs, out, cap);
  return false;
}

bool openpath_git_root(const char *dir, char *out, size_t cap) {
  if (!dir || !*dir || !absolute(dir)) return false;
  char d[OPENPATH_MAX], probe[OPENPATH_MAX + 8];
  snprintf(d, sizeof d, "%s", dir);
  for (;;) {
    size_t l = strlen(d);
    while (l > 1 && d[l - 1] == '/') d[--l] = 0;
    snprintf(probe, sizeof probe, "%s/.git", strcmp(d, "/") ? d : "");
    if (exists(probe)) {
      snprintf(out, cap, "%s", d);
      return true;
    }
    char *slash = strrchr(d, '/');
    if (!slash || slash == d) return false; /* reached the root */
    *slash = 0;
  }
}

/* ---- spelling the command ----------------------------------------------- */

typedef struct {
  char *p;
  size_t n, cap;
  bool full;
} sbuf_t;

static void put(sbuf_t *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->full = true;
    return;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void puts_(sbuf_t *b, const char *s) { put(b, s, strlen(s)); }

/* One shell word. POSIX: single quotes, with a quote inside closed, escaped
 * and reopened. Windows' cmd.exe has no single quotes, so double ones. */
static void quote(sbuf_t *b, const char *s) {
#ifdef _WIN32
  puts_(b, "\"");
  puts_(b, s);
  puts_(b, "\"");
#else
  puts_(b, "'");
  for (const char *c = s; *c; c++) {
    if (*c == '\'')
      puts_(b, "'\\''");
    else
      put(b, c, 1);
  }
  puts_(b, "'");
#endif
}

static void num(sbuf_t *b, long v) {
  char t[24];
  snprintf(t, sizeof t, "%ld", v > 0 ? v : 1);
  puts_(b, t);
}

typedef enum {
  ED_PLAIN,  /* just the path */
  ED_PLUS,   /* +12 path */
  ED_VIM,    /* '+call cursor(12,5)' path */
  ED_PLUSLC, /* +12:5 path */
  ED_NANO,   /* +12,5 path */
  ED_COLON,  /* path:12:5 */
  ED_GOTO,   /* --goto path:12:5 */
} ed_style_t;

static ed_style_t editor_style(const char *editor) {
  /* The program is the first word, and its name is what follows the last
   * slash: `/usr/bin/nvim`, `nvim -p` and `nvim` are all nvim. */
  char name[256];
  size_t i = 0;
  while (editor[i] == ' ') i++;
  size_t s = i;
  while (editor[i] && editor[i] != ' ') {
    if (editor[i] == '/' || editor[i] == '\\') s = i + 1;
    i++;
  }
  size_t n = i - s;
  if (n >= sizeof name) return ED_PLAIN;
  memcpy(name, editor + s, n);
  name[n] = 0;
  char *dot = strrchr(name, '.');
  if (dot && strcasecmp(dot, ".exe") == 0) *dot = 0;

  static const struct {
    const char *name;
    ed_style_t style;
  } known[] = {
      {"vim", ED_VIM},
      {"nvim", ED_VIM},
      {"gvim", ED_VIM},
      {"mvim", ED_VIM},
      {"view", ED_VIM},
      {"vi", ED_PLUS},
      {"vis", ED_PLUS},
      {"mg", ED_PLUS},
      {"jed", ED_PLUS},
      {"ne", ED_PLUS},
      {"joe", ED_PLUS},
      {"nano", ED_NANO},
      {"pico", ED_PLUS},
      {"emacs", ED_PLUSLC},
      {"emacsclient", ED_PLUSLC},
      {"kak", ED_PLUSLC},
      {"micro", ED_PLUSLC},
      {"hx", ED_COLON},
      {"helix", ED_COLON},
      {"subl", ED_COLON},
      {"zed", ED_COLON},
      {"code", ED_GOTO},
      {"code-insiders", ED_GOTO},
      {"codium", ED_GOTO},
      {"cursor", ED_GOTO},
      {"windsurf", ED_GOTO},
  };
  for (size_t k = 0; k < sizeof known / sizeof *known; k++)
    if (strcmp(name, known[k].name) == 0) return known[k].style;
  return ED_PLAIN;
}

bool openpath_command(const char *tmpl, const char *editor, const char *path,
                      long line, long col, char *out, size_t cap) {
  if (!path || !*path || !out || !cap) return false;
  sbuf_t b = {.p = out, .cap = cap};
  out[0] = 0;

  if (tmpl && *tmpl) {
    char dir[OPENPATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir)
      *slash = 0;
    else if (slash)
      slash[1] = 0;
    else
      snprintf(dir, sizeof dir, ".");

    bool said_path = false;
    for (const char *c = tmpl; *c; c++) {
      if (strncmp(c, "{path}", 6) == 0) {
        quote(&b, path);
        said_path = true;
        c += 5;
      } else if (strncmp(c, "{dir}", 5) == 0) {
        quote(&b, dir);
        c += 4;
      } else if (strncmp(c, "{line}", 6) == 0) {
        num(&b, line);
        c += 5;
      } else if (strncmp(c, "{col}", 5) == 0) {
        num(&b, col);
        c += 4;
      } else {
        put(&b, c, 1);
      }
    }
    if (!said_path) {
      puts_(&b, " ");
      quote(&b, path);
    }
    return !b.full;
  }

  if (!editor || !*editor) editor = "vi";
  puts_(&b, editor);
  puts_(&b, " ");
  ed_style_t st = line > 0 ? editor_style(editor) : ED_PLAIN;
  char t[64];
  switch (st) {
  case ED_PLAIN: quote(&b, path); break;
  case ED_PLUS:
    snprintf(t, sizeof t, "+%ld ", line);
    puts_(&b, t);
    quote(&b, path);
    break;
  case ED_VIM:
    if (col > 0)
      snprintf(t, sizeof t, "'+call cursor(%ld,%ld)' ", line, col);
    else
      snprintf(t, sizeof t, "+%ld ", line);
    puts_(&b, t);
    quote(&b, path);
    break;
  case ED_PLUSLC:
  case ED_NANO:
    if (col > 0)
      snprintf(t, sizeof t, "+%ld%c%ld ", line, st == ED_NANO ? ',' : ':', col);
    else
      snprintf(t, sizeof t, "+%ld ", line);
    puts_(&b, t);
    quote(&b, path);
    break;
  case ED_GOTO: puts_(&b, "--goto "); /* fall through */
  case ED_COLON: {
    char lc[OPENPATH_MAX + 48];
    if (col > 0)
      snprintf(lc, sizeof lc, "%s:%ld:%ld", path, line, col);
    else
      snprintf(lc, sizeof lc, "%s:%ld", path, line);
    quote(&b, lc);
    break;
  }
  }
  return !b.full;
}
