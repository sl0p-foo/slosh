/* Fuzz target: the JSON reader (src/jsonval.c).
 *
 * This is what the control API feeds every MSG_CMD line into, so its input
 * is exactly "whatever a client wrote on the socket". Parse, then walk the
 * whole value through the accessors cmd.c uses.
 */
#include "jsonval.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void walk(const jv_t *v, int depth) {
  if (!v || depth > 200) return;
  (void)jv_str(v, "fallback");
  (void)jv_int(v, -1);
  (void)jv_bool(v, false);
  if (v->kind == JV_OBJ) {
    (void)jv_get(v, "no-such-key");
    (void)jv_gets(v, "no-such-key", "fb");
    (void)jv_geti(v, "no-such-key", 7);
    (void)jv_getb(v, "no-such-key", true);
    for (size_t i = 0; i < v->len; i++) {
      (void)jv_get(v, v->keys[i]);
      walk(v->kids[i], depth + 1);
    }
  } else if (v->kind == JV_ARR) {
    for (size_t i = 0; i < v->len; i++) walk(v->kids[i], depth + 1);
  } else if (v->kind == JV_STR) {
    /* the string must really be len bytes and NUL-terminated */
    if (v->str && strlen(v->str) > v->len) abort();
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) return 0;
  memcpy(text, data, size);
  text[size] = '\0';

  jv_t *v = jv_parse(text);
  if (v) {
    walk(v, 0);
    jv_free(v);
  }
  free(text);
  return 0;
}
