/* Finding projects. See project.h for what one is and why nothing watches. */
#define _GNU_SOURCE
#include "project.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "slosh.h"

/* FNV-1a over the resolved path, printed as eight hex digits.
 *
 * Eight because a slug has to fit a purpose (64 bytes, `[A-Za-z0-9_.:/-]`)
 * beside a name people will recognise, and because the thing being avoided is
 * two projects with the same basename -- `~/dev/api` and `~/work/api` -- not an
 * adversary. Cryptography is not what a tab label is for. */
static void hash8(const char *s, char *out) {
  uint32_t h = 2166136261u;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 16777619u;
  }
  snprintf(out, 9, "%08x", h);
}

void project_slug(const char *path, const char *name, char *out, size_t cap) {
  char h[9];
  hash8(path, h);
  /* The name is truncated rather than the hash: the hash is what makes the slug
   * unique, and half a hash is a collision waiting. */
  int room = (int)cap - (int)sizeof "project:" - 9;
  if (room < 1) room = 1;
  snprintf(out, cap, "project:%.*s.%s", room, name, h);
}

/* A directory's own name, without a trailing slash to confuse it. */
static void basename_of(const char *path, char *out, size_t cap) {
  const char *end = path + strlen(path);
  while (end > path && end[-1] == '/') end--;
  const char *start = end;
  while (start > path && start[-1] != '/') start--;
  size_t n = (size_t)(end - start);
  if (n >= cap) n = cap - 1;
  memcpy(out, start, n);
  out[n] = 0;
}

static bool exists(const char *dir, const char *leaf, struct stat *st) {
  char p[1024];
  snprintf(p, sizeof p, "%s/%s", dir, leaf);
  struct stat local;
  return stat(p, st ? st : &local) == 0;
}

/* Fill in a project, or say this directory is not one.
 *
 * Two markers, in order of authority: a layout file means the project said what
 * it needs, and a `.git` means it is a project that has not said yet. The second
 * is why this is worth having at all -- a `~/dev` of forty checkouts and three
 * layout files would otherwise be a picker with three rows. */
static bool describe(const char *path, project_t *p) {
  memset(p, 0, sizeof *p);
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

  char real[PATH_MAX];
  const char *resolved = realpath(path, real) ? real : path;
  snprintf(p->path, sizeof p->path, "%s", resolved);
  basename_of(p->path, p->name, sizeof p->name);
  if (!p->name[0] || p->name[0] == '.')
    return false; /* not `.`, `..`, dotdirs */
  project_slug(p->path, p->name, p->slug, sizeof p->slug);

  struct stat lst;
  if (exists(p->path, PROJECT_LAYOUT_FILE, &lst)) {
    snprintf(p->layout, sizeof p->layout, "%s/%s", p->path,
             PROJECT_LAYOUT_FILE);
    p->mtime = (int64_t)lst.st_mtime;
    return true;
  }
  /* The old spelling, found but never written: see PROJECT_LAYOUT_FILE_OLD. */
  if (exists(p->path, PROJECT_LAYOUT_FILE_OLD, &lst)) {
    snprintf(p->layout, sizeof p->layout, "%s/%s", p->path,
             PROJECT_LAYOUT_FILE_OLD);
    p->mtime = (int64_t)lst.st_mtime;
    return true;
  }
  return exists(p->path, ".git", NULL);
}

static int by_name(const void *a, const void *b) {
  return strcmp(((const project_t *)a)->name, ((const project_t *)b)->name);
}

/* One level of one root. Recurses only into directories that are *not* projects,
 * so a checkout's own subdirectories are never walked. */
static void scan_dir(const char *dir, int depth, project_t *out, size_t max,
                     size_t *n) {
  if (depth <= 0 || *n >= max) return;
  DIR *d = opendir(dir);
  if (!d) return; /* missing or unreadable is the normal case, not an error */
  struct dirent *e;
  while ((e = readdir(d)) && *n < max) {
    if (e->d_name[0] == '.') continue; /* `.`, `..`, and dotdirs */
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    project_t p;
    if (describe(path, &p)) {
      /* Deduplicated by path: two roots may overlap, and one project appearing
       * twice in a picker is a picker that cannot be trusted. */
      bool seen = false;
      for (size_t i = 0; i < *n && !seen; i++)
        seen = strcmp(out[i].path, p.path) == 0;
      if (!seen) out[(*n)++] = p;
      continue;
    }
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
      scan_dir(path, depth - 1, out, max, n);
  }
  closedir(d);
}

size_t project_scan(const char *const *roots, int depth, project_t *out,
                    size_t max) {
  size_t n = 0;
  if (!roots) return 0;
  for (size_t i = 0; roots[i] && *roots[i]; i++) {
    char buf[1024];
    scan_dir(path_expand(roots[i], buf, sizeof buf), depth, out, max, &n);
  }
  qsort(out, n, sizeof *out, by_name);
  return n;
}

bool project_find(const char *const *roots, int depth, const char *name,
                  project_t *out) {
  if (!name || !*name) return false;
  project_t all[PROJECTS_MAX];
  size_t n = project_scan(roots, depth, all, PROJECTS_MAX);

  /* A path is matched against what a scan found rather than described on its
   * own, which is the containment check: a directory no root holds is one this
   * session will not open and will not write a layout into. */
  bool is_path = strchr(name, '/') != NULL;
  char want[PATH_MAX];
  if (is_path) {
    char buf[1024];
    const char *expanded = path_expand(name, buf, sizeof buf);
    if (!realpath(expanded, want)) return false;
  }

  for (size_t i = 0; i < n; i++) {
    bool hit = is_path ? strcmp(all[i].path, want) == 0
                       : strcmp(all[i].name, name) == 0;
    if (!hit) continue;
    *out = all[i];
    return true;
  }
  return false;
}
