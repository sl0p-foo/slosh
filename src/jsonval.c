#define _GNU_SOURCE
#include "jsonval.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *p;
  bool bad;
} P;

static jv_t *parse_value(P *s);

static void skip_ws(P *s) {
  while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r')
    s->p++;
}

static jv_t *jv_new(jv_kind_t k) {
  jv_t *v = calloc(1, sizeof *v);
  v->kind = k;
  return v;
}

static void utf8_put(char **out, unsigned cp) {
  char *o = *out;
  if (cp < 0x80)
    *o++ = (char)cp;
  else if (cp < 0x800) {
    *o++ = (char)(0xc0 | (cp >> 6));
    *o++ = (char)(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    *o++ = (char)(0xe0 | (cp >> 12));
    *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
    *o++ = (char)(0x80 | (cp & 0x3f));
  } else {
    *o++ = (char)(0xf0 | (cp >> 18));
    *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
    *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
    *o++ = (char)(0x80 | (cp & 0x3f));
  }
  *out = o;
}

static unsigned hex4(const char *p) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (unsigned)(c - 'A' + 10);
  }
  return v;
}

static char *parse_string_raw(P *s, size_t *out_len) {
  if (*s->p != '"') {
    s->bad = true;
    return NULL;
  }
  s->p++;
  const char *start = s->p;
  size_t max = strlen(start) + 1;
  char *buf = malloc(max);
  char *o = buf;
  while (*s->p && *s->p != '"') {
    if (*s->p == '\\') {
      s->p++;
      switch (*s->p) {
      case 'n': *o++ = '\n'; break;
      case 't': *o++ = '\t'; break;
      case 'r': *o++ = '\r'; break;
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case '/': *o++ = '/'; break;
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case 'u': {
        unsigned cp = hex4(s->p + 1);
        s->p += 4;
        if (cp >= 0xd800 && cp < 0xdc00 && s->p[1] == '\\' && s->p[2] == 'u') {
          unsigned lo = hex4(s->p + 3);
          s->p += 6;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        }
        utf8_put(&o, cp);
        break;
      }
      default:
        s->bad = true;
        free(buf);
        return NULL;
      }
      s->p++;
    } else {
      *o++ = *s->p++;
    }
  }
  if (*s->p != '"') {
    s->bad = true;
    free(buf);
    return NULL;
  }
  s->p++;
  *o = 0;
  if (out_len) *out_len = (size_t)(o - buf);
  return buf;
}

static jv_t *parse_value(P *s) {
  skip_ws(s);
  char c = *s->p;

  if (c == '{') {
    s->p++;
    jv_t *v = jv_new(JV_OBJ);
    skip_ws(s);
    if (*s->p == '}') {
      s->p++;
      return v;
    }
    for (;;) {
      skip_ws(s);
      char *key = parse_string_raw(s, NULL);
      if (s->bad) {
        jv_free(v);
        return NULL;
      }
      skip_ws(s);
      if (*s->p != ':') {
        s->bad = true;
        free(key);
        jv_free(v);
        return NULL;
      }
      s->p++;
      jv_t *val = parse_value(s);
      if (!val) {
        free(key);
        jv_free(v);
        return NULL;
      }
      v->keys = realloc(v->keys, (v->len + 1) * sizeof *v->keys);
      v->kids = realloc(v->kids, (v->len + 1) * sizeof *v->kids);
      v->keys[v->len] = key;
      v->kids[v->len] = val;
      v->len++;
      skip_ws(s);
      if (*s->p == ',') {
        s->p++;
        continue;
      }
      if (*s->p == '}') {
        s->p++;
        return v;
      }
      s->bad = true;
      jv_free(v);
      return NULL;
    }
  }

  if (c == '[') {
    s->p++;
    jv_t *v = jv_new(JV_ARR);
    skip_ws(s);
    if (*s->p == ']') {
      s->p++;
      return v;
    }
    for (;;) {
      jv_t *val = parse_value(s);
      if (!val) {
        jv_free(v);
        return NULL;
      }
      v->kids = realloc(v->kids, (v->len + 1) * sizeof *v->kids);
      v->kids[v->len++] = val;
      skip_ws(s);
      if (*s->p == ',') {
        s->p++;
        continue;
      }
      if (*s->p == ']') {
        s->p++;
        return v;
      }
      s->bad = true;
      jv_free(v);
      return NULL;
    }
  }

  if (c == '"') {
    size_t len = 0;
    char *str = parse_string_raw(s, &len);
    if (!str) return NULL;
    jv_t *v = jv_new(JV_STR);
    v->str = str;
    v->len = len;
    return v;
  }

  if (strncmp(s->p, "true", 4) == 0) {
    s->p += 4;
    jv_t *v = jv_new(JV_BOOL);
    v->b = true;
    return v;
  }
  if (strncmp(s->p, "false", 5) == 0) {
    s->p += 5;
    jv_t *v = jv_new(JV_BOOL);
    v->b = false;
    return v;
  }
  if (strncmp(s->p, "null", 4) == 0) {
    s->p += 4;
    return jv_new(JV_NULL);
  }

  {
    char *end = NULL;
    double d = strtod(s->p, &end);
    if (end == s->p) {
      s->bad = true;
      return NULL;
    }
    s->p = end;
    jv_t *v = jv_new(JV_NUM);
    v->num = d;
    return v;
  }
}

jv_t *jv_parse(const char *text) {
  P s = {.p = text};
  jv_t *v = parse_value(&s);
  if (!v) return NULL;
  skip_ws(&s);
  if (*s.p) { /* trailing garbage */
    jv_free(v);
    return NULL;
  }
  return v;
}

void jv_free(jv_t *v) {
  if (!v) return;
  for (size_t i = 0; i < v->len; i++) {
    if (v->kids) jv_free(v->kids[i]);
    if (v->keys) free(v->keys[i]);
  }
  free(v->kids);
  free(v->keys);
  free(v->str);
  free(v);
}

const jv_t *jv_get(const jv_t *obj, const char *key) {
  if (!obj || obj->kind != JV_OBJ) return NULL;
  for (size_t i = obj->len; i-- > 0;) /* last wins */
    if (strcmp(obj->keys[i], key) == 0) return obj->kids[i];
  return NULL;
}

const char *jv_str(const jv_t *v, const char *fallback) {
  return v && v->kind == JV_STR ? v->str : fallback;
}

long jv_int(const jv_t *v, long fallback) {
  if (!v) return fallback;
  if (v->kind == JV_NUM) return (long)v->num;
  if (v->kind == JV_STR) return strtol(v->str, NULL, 10);
  return fallback;
}

bool jv_bool(const jv_t *v, bool fallback) {
  return v && v->kind == JV_BOOL ? v->b : fallback;
}

const char *jv_gets(const jv_t *obj, const char *key, const char *fallback) {
  return jv_str(jv_get(obj, key), fallback);
}
long jv_geti(const jv_t *obj, const char *key, long fallback) {
  return jv_int(jv_get(obj, key), fallback);
}
bool jv_getb(const jv_t *obj, const char *key, bool fallback) {
  return jv_bool(jv_get(obj, key), fallback);
}
