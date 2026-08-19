/* Layout files, checking, workspaces, and dumping a session back out. Split from app.c. */
#define _GNU_SOURCE
#include "app.h"

#include <ghostty/vt.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"
#include "json.h"
#include "version.h"
#include "graphics.h"
#include "kdl.h"
#include "app_internal.h"

/* ---- layouts ------------------------------------------------------------ *
 *
 *   layout {
 *       tab name="api" purpose="project:api.a1b2" cwd="/home/user/dev/api" {
 *           pane purpose="agent:main" command="pi" suspended=true
 *           pane split="rows" {
 *               pane command="npm run dev" suspended=true
 *               pane
 *           }
 *       }
 *   }
 *
 * A `pane` with children is a split; `split` on it picks the direction. Every
 * purpose a layout declares is locked, which is the point: identity comes from
 * the layout, not from whatever the program inside decides to print (D8).
 *
 * `base` is the directory the file came from, and a relative `cwd=` resolves
 * against it, so a layout can be checked in beside the project it describes
 * (path_resolve). A layout that arrived as text has no base and keeps the old
 * meaning: relative to wherever the session is.
 */

static node_t *build_pane(app_t *a, const kdl_node_t *node, const char *cwd,
                          const char *base) {
  /* Resolved once here, so both the pane spawned from this node and every
   * child that inherits the value get the same real directory. The buffer
   * outlives the recursion below it: children finish before we return. An
   * inherited value is already resolved, so it is passed through untouched --
   * resolving it twice would re-root a path against itself. */
  char cwdbuf[1024];
  const char *own = kdl_prop(node, "cwd", NULL);
  const char *node_cwd =
      own ? path_resolve(own, base, cwdbuf, sizeof cwdbuf) : cwd;

  /* a split: children, in order, in one direction */
  size_t kids = 0;
  for (size_t i = 0; i < node->nkids; i++)
    if (strcmp(node->kids[i]->name, "pane") == 0) kids++;

  if (kids) {
    node_t *sp = calloc(1, sizeof *sp);
    sp->kind = NODE_SPLIT;
    sp->id = ++a->next_id;
    /* A dumped layout carries the proportions it had; a hand-written one says
     * nothing and means "even", which is what WEIGHT_UNIT is. */
    sp->weight = (int)kdl_prop_int(node, "weight", WEIGHT_UNIT);
    if (sp->weight < WEIGHT_MIN) sp->weight = WEIGHT_UNIT;
    sp->dir = strcmp(kdl_prop(node, "split", "cols"), "rows") == 0 ? SPLIT_ROWS
                                                                   : SPLIT_COLS;
    for (size_t i = 0; i < node->nkids; i++) {
      if (strcmp(node->kids[i]->name, "pane") != 0) continue;
      node_t *kid = build_pane(a, node->kids[i], node_cwd, base);
      if (!kid) continue;
      sp->kids = realloc(sp->kids, (sp->nkids + 1) * sizeof *sp->kids);
      sp->kids[sp->nkids++] = kid;
      kid->parent = sp;
    }
    if (sp->nkids == 0) {
      free(sp);
      return NULL;
    }
    if (sp->nkids == 1) { /* a split of one is just the pane */
      node_t *only = sp->kids[0];
      only->parent = NULL;
      free(sp->kids);
      free(sp);
      return only;
    }
    return sp;
  }

  const char *command = kdl_prop(node, "command", NULL);
  bool suspended = a->force_suspend || kdl_prop_bool(node, "suspended", false);
  const char *argv[4];
  if (command) {
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;
  } else {
    const char *const *shell = default_argv(a);
    argv[0] = shell[0];
    argv[1] = NULL;
    for (size_t i = 1; shell[i] && i < 3; i++) argv[i] = shell[i];
  }

  node_t *leaf = leaf_new_ex(a, command ? argv : default_argv(a), node_cwd,
                             suspended, command ? command : "");
  if (!leaf) return NULL;
  leaf->weight = (int)kdl_prop_int(node, "weight", WEIGHT_UNIT);
  if (leaf->weight < WEIGHT_MIN) leaf->weight = WEIGHT_UNIT;
  /* `focus=true` restores which pane you were in. Recorded on the node and
   * resolved once the tab exists, because focus belongs to the tab. */
  if (kdl_prop_bool(node, "focus", false)) a->restore_focus = leaf;
  const char *purpose = kdl_prop(node, "purpose", NULL);
  if (purpose) {
    sanitise_purpose(purpose, leaf->purpose, sizeof leaf->purpose);
    leaf->purpose_locked = true; /* declared by a layout: in-band cannot win */
  }
  return leaf;
}

bool app_apply_layout(app_t *a, const kdl_node_t *root, bool replace,
                      const char *base, char *err, size_t errcap) {
  a->restore_tab = (size_t)-1;
  const kdl_node_t *lay = kdl_child(root, "layout");
  if (!lay) lay = root; /* allow a bare list of tabs */

  size_t before = a->ntabs;
  size_t made = 0;
  for (size_t i = 0; i < lay->nkids; i++) {
    const kdl_node_t *t = lay->kids[i];
    if (strcmp(t->name, "tab") != 0) continue;

    tab_t *tab = tab_add(a, kdl_prop(t, "name", ""));
    bool active = kdl_prop_bool(t, "active", false);
    const char *purpose = kdl_prop(t, "purpose", NULL);
    if (purpose) {
      sanitise_purpose(purpose, tab->purpose, sizeof tab->purpose);
      tab->purpose_locked = true;
    }

    /* The tab body is a split of its pane children, and the tab's own props
     * are its root's -- including `cwd`, which build_pane reads off the body
     * for itself. `base` as the inherited value is what makes a layout with no
     * `cwd` anywhere in it start in the directory it was checked into. */
    kdl_node_t body = {.name = (char *)"pane",
                       .kids = t->kids,
                       .nkids = t->nkids,
                       .props = t->props,
                       .nprops = t->nprops};
    node_t *tree = build_pane(a, &body, base, base);
    if (!tree) tree = leaf_new(a);
    if (!tree) {
      a->ntabs--;
      if (err) snprintf(err, errcap, "cannot create panes for tab %zu", i + 1);
      return false;
    }
    tab->root = tree;
    tab->focus = a->restore_focus ? a->restore_focus : first_leaf_of(tree);
    a->restore_focus = NULL;
    /* `active=true` restores which tab you were looking at. Resolved by index
     * because the tab was appended to whatever was already there. */
    if (active) a->restore_tab = (size_t)(tab - a->tabs);
    made++;
  }

  if (!made) {
    /* The other half of telling the two documents apart. A file with `theme` or
     * `keys` at the top of it is somebody's config, and "declares no tabs" is true
     * of it in the least useful way -- the same message a layout gets when its own
     * tabs are misspelled. `config_is_setting` answers from the loader's own list,
     * so this cannot drift from what a config actually holds. */
    for (size_t i = 0; i < root->nkids; i++) {
      const kdl_node_t *n = root->kids[i];
      if (n && n->name && config_is_setting(n->name)) {
        if (err)
          snprintf(err, errcap,
                   "this is a config, not a layout: `%s` is a setting",
                   n->name);
        return false;
      }
    }
    if (err) snprintf(err, errcap, "layout declares no tabs");
    return false;
  }

  if (replace) { /* drop the tabs that existed before this layout */
    for (size_t i = 0; i < before; i++) node_free(a->tabs[i].root);
    memmove(&a->tabs[0], &a->tabs[before], made * sizeof *a->tabs);
    a->ntabs = made;
  }
  /* Which tab to land on: the one a dump marked `active`, if it named one,
   * and otherwise the first of what was just built. Setting it unconditionally
   * here is what quietly undid the restore.
   *
   * The index was recorded while the new tabs sat *after* the old ones, and
   * `replace` has just moved them to the front -- so it shifts by exactly the
   * number that were dropped. Off by that, it lands on the wrong tab, which
   * looks like the restore working badly rather than not at all. */
  if (a->restore_tab != (size_t)-1 && replace && a->restore_tab >= before)
    a->restore_tab -= before;
  if (a->restore_tab != (size_t)-1 && a->restore_tab < a->ntabs)
    a->cur = a->restore_tab;
  else
    a->cur = replace ? 0 : before;
  a->restore_tab = (size_t)-1;
  layout(a);
  return true;
}

bool app_apply_layout_text_at(app_t *a, const char *text, bool replace,
                              const char *base, char *err, size_t errcap) {
  kdl_node_t *root = kdl_parse(text, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(a, root, replace, base, err, errcap);
  kdl_free(root);
  return ok;
}

bool app_apply_layout_text(app_t *a, const char *text, bool replace, char *err,
                           size_t errcap) {
  /* No base: text has no directory it came from, so a relative `cwd=` in it
   * keeps meaning what it meant before there was a base to be relative to. */
  return app_apply_layout_text_at(a, text, replace, NULL, err, errcap);
}

bool app_apply_layout_file(app_t *a, const char *path, bool replace, char *err,
                           size_t errcap) {
  /* Expanded here rather than left to a shell: `--layout "~/x.layout.kdl"` in
   * quotes, and every path arriving over the control socket, reach fopen with
   * the tilde still on them. */
  char pathbuf[1024], dirbuf[1024];
  const char *file = path_expand(path, pathbuf, sizeof pathbuf);
  kdl_node_t *root = kdl_parse_file(file, err, errcap);
  if (!root) return false;
  bool ok = app_apply_layout(
      a, root, replace, path_dir(file, dirbuf, sizeof dirbuf), err, errcap);
  kdl_free(root);
  return ok;
}

/* ---- checking one -------------------------------------------------------- *
 *
 * The other half of D2's promise: a document says which one it is, and a name
 * the loader does not read is reported rather than skipped. build_pane() above
 * asks for props by name and ignores the rest, which is right for loading --
 * a session should start -- and useless for the person who wrote `cmd=` where
 * `command=` was meant and got a shell.
 *
 * These lists sit next to the code that reads them on purpose, and
 * `tests/test_layout_check.py` greps this file for every `kdl_prop*(node, ...)`
 * the layout section asks for and fails if one is missing here -- the same
 * arrangement that keeps KNOWN_TOP honest in config.c.
 */

/* Read by build_pane, on a `pane` node or on a tab acting as its own root. */
static const char *const PANE_PROPS[] = {
    "split", "weight", "cwd", "command", "focus", "purpose", "suspended"};
/* Read by app_apply_layout off the tab itself. */
static const char *const TAB_PROPS[] = {"name", "active"};

static bool in_list(const char *const *list, size_t n, const char *name) {
  for (size_t i = 0; i < n; i++)
    if (strcmp(list[i], name) == 0) return true;
  return false;
}

#define NELEM(x) (sizeof(x) / sizeof(*(x)))

typedef struct {
  const char *file;
  layout_msg_t *msgs;
  size_t max, n, dropped;
} lcheck_t;

static void lc_say(lcheck_t *c, int line, const char *fmt, ...) {
  if (c->n >= c->max) {
    c->dropped++;
    return;
  }
  char text[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  snprintf(c->msgs[c->n++], sizeof *c->msgs, "%s:%d: %s", c->file, line, text);
}

/* `true`/`false` and nothing else: kdl_prop_bool falls back silently, so
 * `suspended=yes` is a pane that quietly starts. */
static void lc_bool(lcheck_t *c, const kdl_node_t *n, const char *key) {
  const char *v = kdl_prop(n, key, NULL);
  if (v && strcmp(v, "true") != 0 && strcmp(v, "false") != 0)
    lc_say(c, n->line, "%s takes true or false, not `%s`", key, v);
}

static void lc_props(lcheck_t *c, const kdl_node_t *n, bool is_tab) {
  size_t kids = 0;
  for (size_t i = 0; i < n->nkids; i++)
    if (strcmp(n->kids[i]->name, "pane") == 0) kids++;

  for (size_t i = 0; i < n->nprops; i++) {
    const char *k = n->props[i].key;
    bool known = in_list(PANE_PROPS, NELEM(PANE_PROPS), k) ||
                 (is_tab && in_list(TAB_PROPS, NELEM(TAB_PROPS), k));
    if (!known) {
      lc_say(c, n->line, "unknown %s property: %s", is_tab ? "tab" : "pane", k);
      continue;
    }
    /* A split reads `split=` and its children; a leaf reads what it runs. The
     * wrong half is dropped without a word, which is how `purpose=` on a split
     * comes to tag nothing at all. A tab reads its own name and purpose either
     * way, so those are never the ignored ones. */
    bool leaf_only = strcmp(k, "command") == 0 || strcmp(k, "suspended") == 0 ||
                     strcmp(k, "focus") == 0 ||
                     (!is_tab && strcmp(k, "purpose") == 0);
    if (kids && leaf_only)
      lc_say(c, n->line, "%s is ignored on a %s with panes in it", k,
             is_tab ? "tab" : "pane");
    if (!kids && strcmp(k, "split") == 0)
      lc_say(c, n->line, "split is ignored on a pane with nothing to split");
  }

  const char *dir = kdl_prop(n, "split", NULL);
  if (dir && strcmp(dir, "cols") != 0 && strcmp(dir, "rows") != 0)
    lc_say(c, n->line, "split is cols or rows, not `%s`", dir);

  const char *w = kdl_prop(n, "weight", NULL);
  if (w) {
    char *end = NULL;
    long v = strtol(w, &end, 10);
    if (!end || *end || v < WEIGHT_MIN)
      lc_say(c, n->line, "weight is a number of %d or more, not `%s`",
             WEIGHT_MIN, w);
  }

  lc_bool(c, n, "suspended");
  lc_bool(c, n, "focus");
  if (is_tab) lc_bool(c, n, "active");

  for (size_t i = 0; i < n->nkids; i++) {
    const kdl_node_t *k = n->kids[i];
    if (strcmp(k->name, "pane") != 0) {
      lc_say(c, k->line, "a %s holds panes, not `%s`", is_tab ? "tab" : "pane",
             k->name);
      continue;
    }
    lc_props(c, k, false);
  }
}

size_t layout_check(const kdl_node_t *root, const char *file,
                    layout_msg_t *msgs, size_t max, size_t *dropped) {
  lcheck_t c = {.file = file, .msgs = msgs, .max = max};
  const kdl_node_t *lay = kdl_child(root, "layout");
  size_t tabs = 0;

  for (size_t i = 0; i < root->nkids; i++) {
    const kdl_node_t *n = root->kids[i];
    if (!n || !n->name) continue;
    if (lay && n == lay) continue;
    if (strcmp(n->name, "tab") == 0)
      continue; /* a bare list of tabs is legal */
    if (config_is_setting(n->name)) {
      lc_say(&c, n->line, "this is a config, not a layout: `%s` is a setting",
             n->name);
      continue;
    }
    lc_say(&c, n->line, "unknown node: %s", n->name);
  }

  if (!lay) lay = root;
  for (size_t i = 0; i < lay->nkids; i++) {
    const kdl_node_t *n = lay->kids[i];
    if (!n || !n->name) continue;
    if (strcmp(n->name, "tab") != 0) {
      if (lay != root)
        lc_say(&c, n->line, "a layout holds tabs, not `%s`", n->name);
      continue;
    }
    tabs++;
    lc_props(&c, n, true);
  }

  /* Last, so it reads as the summary it is rather than the first surprise. */
  if (!tabs) lc_say(&c, lay->line, "this layout declares no tabs");
  if (dropped) *dropped = c.dropped;
  return c.n;
}

size_t layout_check_file(const char *path, layout_msg_t *msgs, size_t max,
                         size_t *dropped) {
  char pathbuf[1024];
  const char *file = path_expand(path, pathbuf, sizeof pathbuf);
  const char *base = strrchr(file, '/');
  base = base ? base + 1 : file;

  char err[256] = {0};
  kdl_node_t *root = kdl_parse_file(file, err, sizeof err);
  if (!root) {
    /* kdl reports `line N: what`, with no filename because it never saw one,
     * and `cannot open X` for the other kind of failure -- which carries its
     * own path and no line. Same `file:line: text` shape as everything else
     * either way, so an editor's compile step reads all of them. */
    if (max) {
      int line = 0;
      if (sscanf(err, "line %d:", &line) == 1)
        snprintf(msgs[0], sizeof *msgs, "%s:%d:%s", base, line,
                 strchr(err, ':') + 1);
      else
        snprintf(msgs[0], sizeof *msgs, "%s", err);
    }
    if (dropped) *dropped = 0;
    return max ? 1 : 0;
  }
  size_t n = layout_check(root, base, msgs, max, dropped);
  kdl_free(root);
  return n;
}

/* ---- workspaces ---------------------------------------------------------- *
 *
 * A project is a directory (project.c). A workspace is the tab it occupies here.
 * Membership is the tab's *purpose*, in the `project:` namespace -- the shape
 * this repo has published as a project tab's identity since D3 -- so there is no
 * second answer to "what is this tab" and a dumped layout restores membership
 * for free, because a dump already writes tab purposes and apply-layout already
 * locks them (D8).
 *
 * Opening is idempotent, which is `sl0ppi up`'s property carried over: the same
 * request twice is one workspace, focused. Everything else follows from that --
 * a script can drive it in a loop without asking first, and so can a keystroke.
 */

/* Which roots to scan and how deep, from the config in force. Read fresh each
 * time rather than cached, because saving the config is allowed to change where
 * projects live and a cache would answer with yesterday's ~/dev. */
static const char *const *workspace_roots(int *depth) {
  static const char *roots[PROJECT_ROOTS_MAX + 1];
  size_t n = 0;
  for (; n < CFG.nproject_roots && n < PROJECT_ROOTS_MAX; n++)
    roots[n] = CFG.project_roots[n];
  roots[n] = NULL;
  if (depth) *depth = CFG.project_depth;
  return roots;
}

bool app_project_roots_set(void) { return CFG.nproject_roots > 0; }

size_t app_projects(project_t *out, size_t max) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  return project_scan(roots, depth, out, max);
}

/* The first tab holding this workspace, or NULL: a workspace whose layout
 * declared several tabs has several, and the first is the one to land in. */
static tab_t *workspace_tab(app_t *a, const char *slug) {
  for (size_t i = 0; i < a->ntabs; i++)
    if (strcmp(a->tabs[i].purpose, slug) == 0) return &a->tabs[i];
  return NULL;
}

uint32_t app_workspace_tab(app_t *a, const char *slug) {
  tab_t *t = workspace_tab(a, slug);
  return t ? t->id : 0;
}

/* Make every tab this apply just built a member of the workspace.
 *
 * A tab the layout gave a purpose of its own keeps it and is not a member --
 * honoured rather than overwritten, because overwriting a declared purpose is
 * the one thing D8 forbids. The count comes back so the caller can say so
 * instead of it being a silent surprise. */
static void adopt_tabs(app_t *a, size_t from, const project_t *p,
                       app_workspace_open_t *out) {
  for (size_t i = from; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    if (t->purpose[0] && strncmp(t->purpose, "project:", 8) != 0) {
      out->honoured++;
      continue;
    }
    sanitise_purpose(p->slug, t->purpose, sizeof t->purpose);
    t->purpose_locked = true;
    /* A tab with no name of its own takes the project's: a strip reading
     * `1 2 3` is not navigation, which is the whole complaint workspaces
     * answer. */
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", p->name);
    if (!out->tab) out->tab = t->id;
    out->tabs++;
  }
}

bool app_workspace_find(const char *name, project_t *out) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  return project_find(roots, depth, name, out);
}

bool app_workspace_open(app_t *a, const char *name, bool suspended,
                        app_workspace_open_t *out, char *err, size_t errcap) {
  memset(out, 0, sizeof *out);
  project_t p;
  if (!app_workspace_find(name, &p)) {
    snprintf(err, errcap,
             CFG.nproject_roots ? "no project called %s"
                                : "no project roots: set project_roots in your "
                                  "config (asked for %s)",
             name);
    return false;
  }
  snprintf(out->purpose, sizeof out->purpose, "%s", p.slug);
  snprintf(out->path, sizeof out->path, "%s", p.path);

  /* Already open: focus it. The same request twice is one workspace, and the
   * answer says which branch it took so a caller never has to ask first. */
  tab_t *have = workspace_tab(a, p.slug);
  if (have) {
    out->tab = have->id;
    app_select_tab_id(a, have->id);
    return true;
  }

  /* `suspended` has to be decided before anything spawns: a pane is created
   * suspended or it is created running, and there is no un-running a process.
   * Carried on the app for the duration of one apply, the way restore_focus and
   * restore_tab already are, rather than widening two signatures for it. */
  a->force_suspend = suspended;
  size_t before = a->ntabs;
  bool ok;
  if (p.layout[0]) {
    ok = app_apply_layout_file(a, p.layout, false, err, errcap);
  } else if (CFG.project_layout) {
    /* The base is the *project*, not the directory this file came from: the
     * point of a shared project layout is that `cwd="."` means whichever
     * project is being opened. Relative to itself it would open every project
     * in ~/.config. */
    char buf[1024];
    kdl_node_t *root = kdl_parse_file(
        path_expand(CFG.project_layout, buf, sizeof buf), err, errcap);
    ok = root && app_apply_layout(a, root, false, p.path, err, errcap);
    kdl_free(root);
  } else {
    /* No file anywhere: one pane, your shell, in the project. Which is the
     * least a `.git` with nothing else in it can honestly be opened as. */
    char kdl[256];
    snprintf(kdl, sizeof kdl, "layout { tab { pane } }");
    ok = app_apply_layout_text_at(a, kdl, false, p.path, err, errcap);
  }
  a->force_suspend = false;
  if (!ok) return false;

  adopt_tabs(a, before, &p, out);
  out->created = true;
  if (out->tab) app_select_tab_id(a, out->tab);
  layout(a);
  return true;
}

size_t app_workspace_close(app_t *a, const char *slug) {
  size_t closed = 0;
  /* Re-found each time rather than collected first: closing a tab moves every
   * index after it, and a list of pointers taken beforehand would be stale by
   * the second one. */
  for (;;) {
    tab_t *t = workspace_tab(a, slug);
    if (!t || !app_close_tab(a, t->id)) break;
    closed++;
  }
  return closed;
}

/* Which project a tab belongs to, or is standing in.
 *
 * Three answers in order: the path asked for, the project whose slug this tab
 * already carries, and the directory the focused pane is in. All three are
 * resolved by *scanning*, which is also the containment check -- a directory no
 * root holds is one this session will not write a layout into, and that falls
 * out of asking the same question the picker asks rather than being a rule of
 * its own. */
static bool tab_project(app_t *a, const tab_t *t, const char *path,
                        project_t *out) {
  int depth = 2;
  const char *const *roots = workspace_roots(&depth);
  if (path && *path) return project_find(roots, depth, path, out);

  if (strncmp(t->purpose, "project:", 8) == 0) {
    project_t all[PROJECTS_MAX];
    size_t n = project_scan(roots, depth, all, PROJECTS_MAX);
    for (size_t i = 0; i < n; i++)
      if (strcmp(all[i].slug, t->purpose) == 0) {
        *out = all[i];
        return true;
      }
  }

  if (!t->focus) return false;
  char cwdbuf[4096];
  return project_find(roots, depth,
                      live_cwd(t->focus->pane, cwdbuf, sizeof cwdbuf), out);
}

bool app_workspace_save(app_t *a, uint32_t tab, const char *path, int suspend,
                        bool force, app_workspace_save_t *out, char *err,
                        size_t errcap) {
  memset(out, 0, sizeof *out);
  tab_t *t = tab ? tab_by_id(a, tab) : cur(a);
  if (!t) {
    snprintf(err, errcap, "no such tab");
    return false;
  }

  project_t p;
  if (!tab_project(a, t, path, &p)) {
    snprintf(err, errcap, "this tab is not in a project root");
    return false;
  }
  /* A tab that is already *a* workspace may only be saved into its own project.
   * Saving it elsewhere would leave one tab claiming two projects -- named for
   * one, carrying the other's purpose -- which is the second source of truth
   * this whole design exists to avoid. Almost always it is a `path` typed while
   * the wrong tab was focused, so it is said rather than obeyed. */
  if (strncmp(t->purpose, "project:", 8) == 0 &&
      strcmp(t->purpose, p.slug) != 0) {
    snprintf(
        err, errcap,
        "this tab is another project's workspace (%s): save it from its own"
        " tab, or pass that tab's id",
        t->purpose);
    return false;
  }
  snprintf(out->path, sizeof out->path, "%s/%s", p.path, PROJECT_LAYOUT_FILE);
  out->replaced = p.layout[0] != 0;
  if (out->replaced && !force) {
    snprintf(err, errcap, "%s already has a layout: pass force to replace it",
             p.name);
    return false;
  }

  dump_layout_t o = {
      .tab = t->id, .base = p.path, .suspend = suspend, .for_project = true};
  char *kdl = app_dump_layout(a, &o);
  if (!kdl || !o.tabs) {
    free(kdl);
    snprintf(err, errcap, "nothing to write");
    return false;
  }
  FILE *f = fopen(out->path, "w");
  if (!f) {
    free(kdl);
    snprintf(err, errcap, "cannot write %s", out->path);
    return false;
  }
  /* One line of provenance, and the thing to run when it stops working. No
   * timestamp, for the same reason the dump has none: a file that differs every
   * time is a bad diff, and this one is meant to be committed. */
  fputs("// What this project needs open, written by `save-workspace`.\n"
        "// Checked by `slosh --check`; opened by `open-workspace`.\n",
        f);
  fputs(kdl, f);
  bool wrote = fclose(f) == 0;
  free(kdl);
  if (!wrote) {
    snprintf(err, errcap, "cannot write %s", out->path);
    return false;
  }

  out->panes = o.panes;
  out->suspended = o.suspended;
  /* Saving is also adopting: the tab that wrote the file is that project's
   * workspace from now on, so `C-a W` in a tab you happened to build in a
   * checkout is the whole of onboarding one. */
  if (!t->purpose[0] || strncmp(t->purpose, "project:", 8) == 0) {
    sanitise_purpose(p.slug, t->purpose, sizeof t->purpose);
    t->purpose_locked = true;
    if (!t->name[0]) snprintf(t->name, sizeof t->name, "%s", p.name);
  }
  snprintf(out->purpose, sizeof out->purpose, "%s", t->purpose);
  return true;
}

/* ---- dumping a session back out as a layout ------------------------------
 *
 * The inverse of apply-layout, and the thing that makes a restart survivable:
 * build a fresh binary, dump what you have, quit, come back with `--layout`.
 * contrib/slosh-dev wraps exactly that.
 *
 * What can honestly be restored is the *shape*: tabs, their names and
 * purposes, how the panes are split, in what proportion, in which directory,
 * running what they were started with. What cannot is the state inside a
 * program -- a shell's history, a running vim -- and this does not pretend
 * otherwise. A pane running the session's default shell is dumped as a pane
 * with no command, so it comes back as a shell rather than as a re-run of one.
 */

typedef struct {
  char *buf;
  size_t len, cap;
} strbuf_t;

static void sb_add(strbuf_t *b, const char *fmt, ...) {
  va_list ap;
  for (;;) {
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < b->cap - b->len) {
      b->len += (size_t)n;
      return;
    }
    b->cap = b->cap ? b->cap * 2 : 1024;
    while (b->cap - b->len <= (size_t)n) b->cap *= 2;
    b->buf = realloc(b->buf, b->cap);
  }
}

/* KDL strings are double-quoted, so the two characters that end or escape one
 * have to be escaped themselves. A path can contain both. */
static void sb_quoted(strbuf_t *b, const char *key, const char *val) {
  if (!val || !*val) return;
  sb_add(b, " %s=\"", key);
  for (const char *p = val; *p; p++) {
    if (*p == '"' || *p == '\\')
      sb_add(b, "\\%c", *p);
    else if ((unsigned char)*p >= 0x20)
      sb_add(b, "%c", *p);
  }
  sb_add(b, "\"");
}

/* Where the program in the pane is *now*, which after any amount of `cd` is
 * not where it was started. The kernel knows; nothing else does. */
const char *live_cwd(const pane_t *p, char *buf, size_t cap) {
  pid_t pid = pane_pid(p);
  if (pid > 0) {
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/cwd", (int)pid);
    ssize_t n = readlink(link, buf, cap - 1);
    if (n > 0) {
      buf[n] = 0;
      return buf;
    }
  }
  return pane_start_cwd(p); /* not running, or not Linux: what it was given */
}

/* Everything one dump needs to know, so dump_node does not have to ask the app
 * which tab it is in the middle of. The focused pane belongs to *its* tab, not
 * to the one you happen to be looking at -- reading `cur(a)->focus` here meant
 * a dump of three tabs restored the focus of one. */
typedef struct {
  const tab_t *tab;
  const char *base;
  int suspend;
  size_t panes, suspended;
} dumpctx_t;

/* The command this pane is written back out with, or NULL for a plain shell.
 *
 * Two sources, in this order. The **label** is what a layout told this pane to
 * run, and it outranks everything: it survives the program exiting (D14), and
 * re-saving a project must not degrade `npm run dev` into whatever node's argv
 * happens to look like this minute. Failing that, **what the pane's terminal is
 * actually running** -- because a pane you split and typed a command into had
 * nothing to say for itself and came back as a bare shell, which made setting a
 * project up by hand and writing it down two different jobs instead of one.
 *
 * An ephemeral pane is a task that happened to be open when the dump was taken,
 * and is written with no command either way: restoring it would reopen somebody's
 * editor on a file they finished with. */
static const char *dump_command(const node_t *n, char *buf, size_t cap) {
  if (pane_ephemeral(n->pane)) return NULL;
  const char *label = pane_label(n->pane);
  if (label && *label) return label;
  return pane_foreground(n->pane, buf, cap);
}

/* Whether this pane is written as one that has not started yet.
 *
 * `as-is` is the honest answer for a session dump: what is suspended now is
 * suspended in the file. It is the wrong answer for a project's layout, where
 * the pane running the dev server you started this morning would start one on
 * every open -- which is the thing `suspended` exists to prevent. So a saved
 * project defaults to `commands`: a pane that has a command is written asleep, a
 * shell is not. Same distinction, and the same word, as `keep_dead`.
 *
 * "Has a command" is whatever the file is about to say, captured or declared, so
 * the two cannot disagree -- a pane written with a `command=` and no
 * `suspended=true` under this policy would start a dev server on every open. */
static bool dump_suspended(const char *command, const node_t *n, int policy) {
  switch (policy) {
  case DUMP_SUSPEND_NONE: return false;
  case DUMP_SUSPEND_ALL: return true;
  case DUMP_SUSPEND_COMMANDS: return command && *command;
  default: return pane_suspended(n->pane);
  }
}

/* `split=` belongs on the node that *has* the children -- that is where
 * build_pane() reads it -- and a tab's own props are the props of its root,
 * so a root split says so on the tab. Getting this backwards produces a file
 * that loads without complaint and rebuilds the wrong tree. */
static void dump_node(node_t *n, strbuf_t *b, int depth, dumpctx_t *ctx) {
  char pad[64];
  int p = depth * 4 < 60 ? depth * 4 : 60;
  memset(pad, ' ', (size_t)p);
  pad[p] = 0;

  sb_add(b, "%spane", pad);
  /* A weight is a share of the parent, so the root of a tab has none. */
  if (n->parent) sb_add(b, " weight=%d", n->weight);

  if (n->kind == NODE_SPLIT) {
    sb_add(b, " split=\"%s\" {\n", n->dir == SPLIT_ROWS ? "rows" : "cols");
    for (size_t i = 0; i < n->nkids; i++)
      dump_node(n->kids[i], b, depth + 1, ctx);
    sb_add(b, "%s}\n", pad);
    return;
  }

  ctx->panes++;
  char cwdbuf[4096];
  /* Relative to the project when there is one, so the file is the same file on
   * another machine. A directory outside the base stays absolute: it is not a
   * fact about the project. */
  sb_quoted(b, "cwd",
            path_relative(live_cwd(n->pane, cwdbuf, sizeof cwdbuf), ctx->base));
  char cmdbuf[4096];
  const char *command = dump_command(n, cmdbuf, sizeof cmdbuf);
  sb_quoted(b, "command", command);
  sb_quoted(b, "purpose", n->purpose);
  if (dump_suspended(command, n, ctx->suspend)) {
    sb_add(b, " suspended=true");
    ctx->suspended++;
  }
  if (n == ctx->tab->focus) sb_add(b, " focus=true");
  sb_add(b, "\n");
}

char *app_dump_layout(app_t *a, dump_layout_t *o) {
  dump_layout_t all = {0};
  if (!o) o = &all;
  o->tabs = o->panes = o->suspended = 0;

  strbuf_t b = {0};
  sb_add(&b, "layout {\n"); /* no timestamp: a layout that differs every time
                             * is a bad diff */
  for (size_t i = 0; i < a->ntabs; i++) {
    tab_t *t = &a->tabs[i];
    if (o->tab && t->id != o->tab) continue;
    dumpctx_t ctx = {.tab = t, .base = o->base, .suspend = o->suspend};
    sb_add(&b, "    tab");
    sb_quoted(&b, "name", t->name);
    /* The workspace's own purpose is derived from where the project is, so a
     * file that lives there does not repeat it -- and a copy of the project
     * elsewhere becomes its own workspace rather than claiming this one's. */
    if (!(o->for_project && strncmp(t->purpose, "project:", 8) == 0))
      sb_quoted(&b, "purpose", t->purpose);
    /* Which tab you were looking at is a fact about a session. Asked for one
     * tab, the answer is that tab, and `active` would be noise in a file
     * checked in beside a project. */
    if (!o->tab && i == a->cur) sb_add(&b, " active=true");
    /* The tab's props are its root's props, so a root that is a split says
     * which way it goes here rather than on a `pane` node of its own. */
    if (t->root && t->root->kind == NODE_SPLIT)
      sb_add(&b, " split=\"%s\"", t->root->dir == SPLIT_ROWS ? "rows" : "cols");
    sb_add(&b, " {\n");
    if (t->root) {
      if (t->root->kind == NODE_SPLIT)
        for (size_t k = 0; k < t->root->nkids; k++)
          dump_node(t->root->kids[k], &b, 2, &ctx);
      else
        dump_node(t->root, &b, 2, &ctx);
    }
    sb_add(&b, "    }\n");
    o->tabs++;
    o->panes += ctx.panes;
    o->suspended += ctx.suspended;
  }
  sb_add(&b, "}\n");
  return b.buf;
}
