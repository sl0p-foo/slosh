#define _GNU_SOURCE
#include "kdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *p;
  int line;
  char *err;
  size_t errcap;
  bool failed;
} K;

static void fail(K *k, const char *msg) {
  if (k->failed) return;
  k->failed = true;
  if (k->err) snprintf(k->err, k->errcap, "line %d: %s", k->line, msg);
}

static kdl_node_t *node_new(const char *name, size_t len, int line) {
  kdl_node_t *n = calloc(1, sizeof *n);
  n->name = strndup(name, len);
  n->line = line;
  return n;
}

void kdl_free(kdl_node_t *n) {
  if (!n) return;
  free(n->name);
  for (size_t i = 0; i < n->nargs; i++) free(n->args[i]);
  free(n->args);
  for (size_t i = 0; i < n->nprops; i++) {
    free(n->props[i].key);
    free(n->props[i].val);
  }
  free(n->props);
  for (size_t i = 0; i < n->nkids; i++) kdl_free(n->kids[i]);
  free(n->kids);
  free(n);
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static bool ident_char(char c) {
  return !is_space(c) && c != '\n' && c != '{' && c != '}' && c != '=' &&
         c != ';' && c != '"' && c != 0;
}

/* whitespace, comments, and (when eat_newlines) blank lines */
static void skip(K *k, bool eat_newlines) {
  for (;;) {
    char c = *k->p;
    if (is_space(c)) {
      k->p++;
    } else if (c == '\n') {
      if (!eat_newlines) return;
      k->line++;
      k->p++;
    } else if (c == '/' && k->p[1] == '/') {
      while (*k->p && *k->p != '\n') k->p++;
    } else if (c == '/' && k->p[1] == '*') {
      k->p += 2;
      while (*k->p && !(*k->p == '*' && k->p[1] == '/')) {
        if (*k->p == '\n') k->line++;
        k->p++;
      }
      if (*k->p) k->p += 2;
    } else if (c == '\\' && k->p[1] == '\n') { /* line continuation */
      k->p += 2;
      k->line++;
    } else {
      return;
    }
  }
}

static char *parse_string(K *k) {
  k->p++; /* opening quote */
  size_t cap = 64, len = 0;
  char *out = malloc(cap);
  while (*k->p && *k->p != '"') {
    char c = *k->p++;
    if (c == '\\') {
      /* A backslash as the last byte must not step past the terminator:
       * consuming the NUL would leave the loop reading out of bounds. Break
       * and let the unterminated-string check below report it. */
      char e = *k->p;
      if (!e) break;
      k->p++;
      switch (e) {
      case 'n': c = '\n'; break;
      case 't': c = '\t'; break;
      case 'r': c = '\r'; break;
      case 'e': c = 0x1b; break;
      case '\\': c = '\\'; break;
      case '"': c = '"'; break;
      default: c = e; break;
      }
    }
    if (c == '\n') k->line++;
    if (len + 2 > cap) {
      cap *= 2;
      out = realloc(out, cap);
    }
    out[len++] = c;
  }
  if (*k->p != '"') {
    fail(k, "unterminated string");
    free(out);
    return NULL;
  }
  k->p++;
  out[len] = 0;
  return out;
}

/* A bare value: number, bool, or unquoted word. */
static char *parse_bare(K *k) {
  const char *start = k->p;
  while (ident_char(*k->p)) k->p++;
  if (k->p == start) {
    fail(k, "expected a value");
    return NULL;
  }
  return strndup(start, (size_t)(k->p - start));
}

static char *parse_value(K *k) {
  return *k->p == '"' ? parse_string(k) : parse_bare(k);
}

static void add_arg(kdl_node_t *n, char *v) {
  n->args = realloc(n->args, (n->nargs + 1) * sizeof *n->args);
  n->args[n->nargs++] = v;
}

static void add_prop(kdl_node_t *n, char *key, char *val) {
  n->props = realloc(n->props, (n->nprops + 1) * sizeof *n->props);
  n->props[n->nprops].key = key;
  n->props[n->nprops].val = val;
  n->nprops++;
}

static void add_kid(kdl_node_t *n, kdl_node_t *kid) {
  n->kids = realloc(n->kids, (n->nkids + 1) * sizeof *n->kids);
  n->kids[n->nkids++] = kid;
}

static bool parse_nodes(K *k, kdl_node_t *parent, bool in_block);

static bool parse_node(K *k, kdl_node_t *parent) {
  const char *start = k->p;
  while (ident_char(*k->p)) k->p++;
  if (k->p == start) {
    fail(k, "expected a node name");
    return false;
  }
  kdl_node_t *n = node_new(start, (size_t)(k->p - start), k->line);

  for (;;) {
    skip(k, false);
    char c = *k->p;
    if (c == 0 || c == '\n' || c == ';' || c == '}') break;
    if (c == '{') {
      k->p++;
      k->line += 0;
      if (!parse_nodes(k, n, true)) {
        kdl_free(n);
        return false;
      }
      break;
    }

    /* an argument, or a property if an '=' follows the word */
    const char *save = k->p;
    char *first = parse_value(k);
    if (!first) {
      kdl_free(n);
      return false;
    }
    if (*k->p == '=') {
      k->p++;
      char *val = parse_value(k);
      if (!val) {
        free(first);
        kdl_free(n);
        return false;
      }
      add_prop(n, first, val);
    } else {
      (void)save;
      add_arg(n, first);
    }
  }

  add_kid(parent, n);
  return true;
}

static bool parse_nodes(K *k, kdl_node_t *parent, bool in_block) {
  for (;;) {
    skip(k, true);
    if (*k->p == 0) {
      if (in_block) {
        fail(k, "unclosed {");
        return false;
      }
      return true;
    }
    if (*k->p == '}') {
      k->p++;
      if (!in_block) {
        fail(k, "unexpected }");
        return false;
      }
      return true;
    }
    if (*k->p == ';') {
      k->p++;
      continue;
    }
    if (!parse_node(k, parent)) return false;
    if (k->failed) return false;
  }
}

kdl_node_t *kdl_parse(const char *text, char *err, size_t errcap) {
  K k = {.p = text, .line = 1, .err = err, .errcap = errcap};
  kdl_node_t *root = node_new("", 0, 1);
  if (!parse_nodes(&k, root, false) || k.failed) {
    kdl_free(root);
    return NULL;
  }
  return root;
}

kdl_node_t *kdl_parse_file(const char *path, char *err, size_t errcap) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (err) snprintf(err, errcap, "cannot open %s", path);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  size_t got = fread(buf, 1, (size_t)n, f);
  buf[got] = 0;
  fclose(f);
  kdl_node_t *root = kdl_parse(buf, err, errcap);
  free(buf);
  return root;
}

const kdl_node_t *kdl_child(const kdl_node_t *n, const char *name) {
  if (!n) return NULL;
  for (size_t i = 0; i < n->nkids; i++)
    if (strcmp(n->kids[i]->name, name) == 0) return n->kids[i];
  return NULL;
}

const char *kdl_arg(const kdl_node_t *n, size_t i, const char *fallback) {
  return n && i < n->nargs ? n->args[i] : fallback;
}

long kdl_arg_int(const kdl_node_t *n, size_t i, long fallback) {
  const char *v = kdl_arg(n, i, NULL);
  if (!v) return fallback;
  char *end = NULL;
  long out = strtol(v, &end, 10);
  return end && end != v ? out : fallback;
}

bool kdl_arg_bool(const kdl_node_t *n, size_t i, bool fallback) {
  const char *v = kdl_arg(n, i, NULL);
  if (!v) return fallback;
  if (strcmp(v, "true") == 0) return true;
  if (strcmp(v, "false") == 0) return false;
  return fallback;
}

const char *kdl_prop(const kdl_node_t *n, const char *key,
                     const char *fallback) {
  if (!n) return fallback;
  for (size_t i = n->nprops; i-- > 0;) /* last wins */
    if (strcmp(n->props[i].key, key) == 0) return n->props[i].val;
  return fallback;
}

long kdl_prop_int(const kdl_node_t *n, const char *key, long fallback) {
  const char *v = kdl_prop(n, key, NULL);
  if (!v) return fallback;
  char *end = NULL;
  long out = strtol(v, &end, 10);
  return end && end != v ? out : fallback;
}

bool kdl_prop_bool(const kdl_node_t *n, const char *key, bool fallback) {
  const char *v = kdl_prop(n, key, NULL);
  if (!v) return fallback;
  if (strcmp(v, "true") == 0) return true;
  if (strcmp(v, "false") == 0) return false;
  return fallback;
}
