/* Fuzz target: the KDL parser (src/kdl.c).
 *
 * Parses arbitrary bytes as a document, and when a tree comes back walks all
 * of it through the same accessors real callers use, so a node the parser
 * built wrong (NULL name, dangling arg, bad count) is dereferenced here
 * rather than later in config load.
 */
#include "kdl.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void walk(const kdl_node_t *n, int depth) {
  if (!n || depth > 200) return;
  (void)kdl_arg(n, 0, "fallback");
  (void)kdl_arg(n, n->nargs, "past-the-end");
  (void)kdl_arg_int(n, 0, -1);
  (void)kdl_arg_bool(n, 0, false);
  (void)kdl_prop(n, "key", NULL);
  (void)kdl_prop_int(n, "key", -1);
  (void)kdl_prop_bool(n, "key", true);
  for (size_t i = 0; i < n->nprops; i++) {
    (void)kdl_prop(n, n->props[i].key, NULL);
    (void)kdl_prop_int(n, n->props[i].key, 0);
    (void)kdl_prop_bool(n, n->props[i].key, false);
  }
  for (size_t i = 0; i < n->nkids; i++) {
    (void)kdl_child(n, n->kids[i]->name);
    walk(n->kids[i], depth + 1);
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  char *text = malloc(size + 1);
  if (!text) return 0;
  memcpy(text, data, size);
  text[size] = '\0';

  char err[256];
  kdl_node_t *root = kdl_parse(text, err, sizeof err);
  if (root) {
    walk(root, 0);
    kdl_free(root);
  }
  free(text);
  return 0;
}
