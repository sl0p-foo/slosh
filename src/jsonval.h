/* A small JSON reader, for the control API (D3). We already emit JSON; this
 * is the other half. Deliberately minimal: no streaming, no comments, no
 * duplicate-key policy beyond "last wins". */
#ifndef SL0PTTY_JSONVAL_H
#define SL0PTTY_JSONVAL_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
  JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ,
} jv_kind_t;

typedef struct jv jv_t;

struct jv {
  jv_kind_t kind;
  bool b;
  double num;
  char *str;   /* JV_STR: NUL-terminated, UTF-8 */
  size_t len;  /* JV_STR bytes; JV_ARR/JV_OBJ element count */
  jv_t **kids; /* JV_ARR/JV_OBJ values */
  char **keys; /* JV_OBJ keys */
};

/* NULL on malformed input. */
jv_t *jv_parse(const char *text);
void jv_free(jv_t *v);

const jv_t *jv_get(const jv_t *obj, const char *key);
const char *jv_str(const jv_t *v, const char *fallback);
long jv_int(const jv_t *v, long fallback);
bool jv_bool(const jv_t *v, bool fallback);

/* Convenience: obj.key as a string/int/bool, with a fallback. */
const char *jv_gets(const jv_t *obj, const char *key, const char *fallback);
long jv_geti(const jv_t *obj, const char *key, long fallback);
bool jv_getb(const jv_t *obj, const char *key, bool fallback);

#endif /* SL0PTTY_JSONVAL_H */
