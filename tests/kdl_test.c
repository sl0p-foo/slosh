/* Parser cases for the KDL subset. Pure input -> tree, so pure tests. */
#include "kdl.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(const char *name, bool cond, const char *detail) {
  if (!cond) fails++;
  printf("%s %-44s %s\n", cond ? "ok  " : "FAIL", name, cond ? "" : detail);
}

static const kdl_node_t *top(const kdl_node_t *root, const char *name) {
  return kdl_child(root, name);
}

int main(void) {
  char err[256];

  {
    const char *doc =
        "// a comment\n"
        "gap 1\n"
        "rounded true\n"
        "title \"hello world\"\n"
        "min_pane cols=24 rows=6\n"
        "/* block\n   comment */\n"
        "theme {\n"
        "  frame_focus \"#ff5fd7\"\n"
        "  nested { deep \"value\" }\n"
        "}\n"
        "oneline { a \"1\"; b \"2\" }\n";
    kdl_node_t *root = kdl_parse(doc, err, sizeof err);
    ok("a document parses", root != NULL, err);
    if (!root) return 1;

    ok("integer argument", kdl_arg_int(top(root, "gap"), 0, -1) == 1, "");
    ok("boolean argument", kdl_arg_bool(top(root, "rounded"), 0, false), "");
    ok("quoted string with a space",
       strcmp(kdl_arg(top(root, "title"), 0, ""), "hello world") == 0, "");
    ok("properties", kdl_prop_int(top(root, "min_pane"), "cols", 0) == 24 &&
                         kdl_prop_int(top(root, "min_pane"), "rows", 0) == 6, "");
    ok("comments are skipped", top(root, "gap") && top(root, "theme"), "");

    const kdl_node_t *theme = top(root, "theme");
    ok("children", theme && theme->nkids == 2, "");
    ok("child values",
       strcmp(kdl_arg(kdl_child(theme, "frame_focus"), 0, ""), "#ff5fd7") == 0, "");
    ok("nesting",
       strcmp(kdl_arg(kdl_child(kdl_child(theme, "nested"), "deep"), 0, ""),
              "value") == 0, "");

    const kdl_node_t *one = top(root, "oneline");
    ok("semicolons separate single-line children", one && one->nkids == 2,
       one ? "wrong count" : "missing");
    kdl_free(root);
  }

  {
    /* Escapes, and the fork's hard-won lesson that a single-line node without
     * a terminator is a parse error rather than a silent surprise. */
    kdl_node_t *root = kdl_parse("a \"x\\ty\\nz\\\"q\"\n", err, sizeof err);
    ok("string escapes", root && strcmp(kdl_arg(top(root, "a"), 0, ""),
                                        "x\ty\nz\"q") == 0, err);
    kdl_free(root);
  }

  {
    kdl_node_t *root = kdl_parse("keys {\n bind \"ctrl+a\" \"quit\"\n", err,
                                 sizeof err);
    ok("an unclosed block is an error, with a line number",
       root == NULL && strstr(err, "line") != NULL, err);
    kdl_free(root);
  }

  {
    kdl_node_t *root = kdl_parse("a \"unterminated\n", err, sizeof err);
    ok("an unterminated string is an error", root == NULL, err);
    kdl_free(root);
  }

  {
    kdl_node_t *root = kdl_parse("", err, sizeof err);
    ok("an empty document is valid", root != NULL && root->nkids == 0, err);
    kdl_free(root);
  }

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all green", fails);
  return fails ? 1 : 0;
}
