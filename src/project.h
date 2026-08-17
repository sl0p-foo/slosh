/* Projects on disk, and the workspaces they open as.
 *
 * A *project* is a directory somebody works in. A *workspace* is the tab it
 * currently occupies in a session. This file is about the first: finding them,
 * naming them, and saying where each one's layout lives. The second lives in
 * app.c, because a workspace is tabs.
 *
 * The list is derived when it is asked for rather than watched. One readdir per
 * root plus two faccessat per entry is forty checkouts for eighty-one syscalls,
 * which is nothing next to the keystroke that asked -- and an answer nobody
 * remembered cannot be stale, which no watcher can promise on a bind mount.
 */
#ifndef SL0PPTY_PROJECT_H
#define SL0PPTY_PROJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The file a project uses to say what it needs. One name, so there is no
 * precedence question: a layout file by D2's rule, checked by `--check`, and
 * highlighted by anything that knows KDL. */
#define PROJECT_LAYOUT_FILE "sl0ppty.layout.kdl"

/* How many projects one session will look at. A picker is not a filesystem
 * browser: past this many the answer to "which project" is a search, not a
 * list, and the list is what this is for. */
#define PROJECTS_MAX 128

typedef struct {
  char name[64];  /* the directory's own name, which is what people call it */
  char path[512]; /* absolute, resolved */
  char slug[64];  /* `project:name.hash8` -- the purpose a workspace carries */
  /* The project's own layout file, or "" for one inferred from a `.git`. The
   * distinction is the whole of what "declared" means here, and the picker
   * shows it: a project with no layout is an invitation to save one. */
  char layout[512];
  int64_t mtime; /* of `layout`, 0 when there is none. Drift is derivable from
                  * this and a pane's `since` without anything storing it. */
} project_t;

/* Every project under `roots`, sorted by name. `roots` is a NUL-terminated
 * array of directories; `depth` is how many levels below each to look, and a
 * directory that *is* a project is never descended into -- which is what keeps
 * this off node_modules without a rule about node_modules.
 *
 * A root that does not exist is not an error. Neither is one with nothing in
 * it: the setting is a statement of where you keep things, not a promise that
 * you have any. */
size_t project_scan(const char *const *roots, int depth, project_t *out,
                    size_t max);

/* One project by name, or by path when `name` looks like one (it has a `/`).
 * False when no root holds it -- which is also the containment check, since a
 * path that no root holds is a path this session will not write to. */
bool project_find(const char *const *roots, int depth, const char *name,
                  project_t *out);

/* The purpose a workspace for this directory carries: `project:<name>.<hash>`
 * of the resolved path. Hashed on the path rather than the name so that two
 * worktrees of one repo are two workspaces, which is what they are. */
void project_slug(const char *path, const char *name, char *out, size_t cap);

#endif /* SL0PPTY_PROJECT_H */
