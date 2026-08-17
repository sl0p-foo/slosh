#define _GNU_SOURCE
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void reserve(json_t *j, size_t n) {
  if (j->len + n <= j->cap) return;
  size_t cap = j->cap ? j->cap : 4096;
  while (cap < j->len + n) cap *= 2;
  j->buf = realloc(j->buf, cap);
  j->cap = cap;
}

static void raw(json_t *j, const char *s, size_t n) {
  reserve(j, n + 1);
  memcpy(j->buf + j->len, s, n);
  j->len += n;
  j->buf[j->len] = 0;
}

static void rawz(json_t *j, const char *s) { raw(j, s, strlen(s)); }

void json_init(json_t *j) {
  memset(j, 0, sizeof *j);
  reserve(j, 4096);
  j->buf[0] = 0;
}

void json_free(json_t *j) {
  free(j->buf);
  memset(j, 0, sizeof *j);
}

static void sep(json_t *j, const char *key) {
  if (j->need_comma) rawz(j, ",");
  if (key) {
    rawz(j, "\"");
    rawz(j, key);
    rawz(j, "\":");
  }
  j->need_comma = true;
}

static void escaped(json_t *j, const char *s, size_t len) {
  rawz(j, "\"");
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"': rawz(j, "\\\""); break;
    case '\\': rawz(j, "\\\\"); break;
    case '\n': rawz(j, "\\n"); break;
    case '\r': rawz(j, "\\r"); break;
    case '\t': rawz(j, "\\t"); break;
    default:
      if (c < 0x20) {
        char b[8];
        snprintf(b, sizeof b, "\\u%04x", c);
        rawz(j, b);
      } else {
        raw(j, (const char *)&c, 1); /* UTF-8 passes through */
      }
    }
  }
  rawz(j, "\"");
}

void json_obj_open(json_t *j, const char *key) {
  sep(j, key);
  rawz(j, "{");
  j->need_comma = false;
}

void json_obj_close(json_t *j) {
  rawz(j, "}");
  j->need_comma = true;
}

void json_arr_open(json_t *j, const char *key) {
  sep(j, key);
  rawz(j, "[");
  j->need_comma = false;
}

void json_arr_close(json_t *j) {
  rawz(j, "]");
  j->need_comma = true;
}

void json_str(json_t *j, const char *key, const char *val, size_t len) {
  sep(j, key);
  escaped(j, val, len);
}

void json_int(json_t *j, const char *key, long long val) {
  sep(j, key);
  char b[32];
  snprintf(b, sizeof b, "%lld", val);
  rawz(j, b);
}

void json_bool(json_t *j, const char *key, bool val) {
  sep(j, key);
  rawz(j, val ? "true" : "false");
}

void json_null(json_t *j, const char *key) {
  sep(j, key);
  rawz(j, "null");
}
