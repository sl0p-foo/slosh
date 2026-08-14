/* A hand-rolled KDL subset (D2).
 *
 * Enough of https://kdl.dev for configuration and layouts, and no more:
 *
 *   name arg1 "arg two" key=value key2=3 {
 *       child "x"; child "y"     // single-line children need ; terminators
 *   }
 *   // line comments, and block comments
 *
 * Values are kept as strings and converted on read, which keeps the parser
 * small and puts type decisions where the meaning is.
 */
#ifndef SL0PPTY_KDL_H
#define SL0PPTY_KDL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *key;
  char *val;
} kdl_prop_t;

typedef struct kdl_node {
  char *name;
  char **args;
  size_t nargs;
  kdl_prop_t *props;
  size_t nprops;
  struct kdl_node **kids;
  size_t nkids;
  int line;
} kdl_node_t;

/* Parses a document into a synthetic root node whose children are the
 * top-level nodes. Returns NULL and fills err (if given) on a syntax error. */
kdl_node_t *kdl_parse(const char *text, char *err, size_t errcap);
kdl_node_t *kdl_parse_file(const char *path, char *err, size_t errcap);
void kdl_free(kdl_node_t *n);

const kdl_node_t *kdl_child(const kdl_node_t *n, const char *name);
const char *kdl_arg(const kdl_node_t *n, size_t i, const char *fallback);
long kdl_arg_int(const kdl_node_t *n, size_t i, long fallback);
bool kdl_arg_bool(const kdl_node_t *n, size_t i, bool fallback);
const char *kdl_prop(const kdl_node_t *n, const char *key, const char *fallback);
long kdl_prop_int(const kdl_node_t *n, const char *key, long fallback);
bool kdl_prop_bool(const kdl_node_t *n, const char *key, bool fallback);

#endif /* SL0PPTY_KDL_H */
