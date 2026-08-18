/* Paths as people write them.
 *
 * `~` is a shell thing: by the time a path reaches us from a config or layout
 * file no shell has touched it, so `cwd="~/dev/api"` was being handed to
 * chdir() literally, failing, and leaving the pane in whatever directory the
 * session happened to be in. Silently \u2014 which is the worst way for a path to
 * be wrong, because the pane still starts and still works.
 */
#include "slosh.h"

#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *path_expand(const char *path, char *buf, size_t cap) {
  if (!path) return NULL;
  /* `~` and `~/...` only. `~user` needs the password database and is not
   * worth linking it in for; it is returned unchanged, which is exactly what
   * it did before, rather than being half-expanded into something wrong. */
  if (path[0] != '~' || (path[1] && path[1] != '/')) return path;
  const char *home = getenv("HOME");
  if (!home || !*home) return path;
  snprintf(buf, cap, "%s%s", home, path + 1);
  return buf;
}

/* A path as a layout file meant it, against the directory that file lives in.
 *
 * A project's layout is checked in with the project, so it cannot name absolute
 * directories: `cwd="/home/user/dev/api"` is right on exactly one machine and
 * wrong in a worktree of the same repo. A relative path therefore resolves
 * against the file that wrote it -- the rule `include` already follows for
 * configs, applied to the other half of the same syntax.
 *
 * `base` NULL is the old behaviour on purpose: a layout arriving as text over
 * the control socket was written by whoever sent it and has no directory to be
 * relative to, so it keeps meaning what it meant. */
const char *path_resolve(const char *path, const char *base, char *buf,
                         size_t cap) {
  if (!path) return NULL;
  if (path[0] == '~') return path_expand(path, buf, cap);
  if (path[0] == '/' || !base || !*base) return path;
  /* `.` is the project directory itself, which is what a dump writes for the
   * pane that started there -- joined naively it would be `/home/you/dev/api/.`,
   * which works and reads like a bug. */
  if (strcmp(path, ".") == 0) return base;
  if (strncmp(path, "./", 2) == 0) path += 2;
  snprintf(buf, cap, "%s/%s", base, path);
  return buf;
}

/* The directory a file lives in: what `path_resolve` wants as its base. A path
 * with no `/` in it is in the current one, which is `.` rather than `""` --
 * an empty base means "no base" to everything downstream. */
const char *path_dir(const char *path, char *buf, size_t cap) {
  if (!path) return NULL;
  const char *slash = strrchr(path, '/');
  if (!slash) return ".";
  if (slash == path) return "/";
  size_t n = (size_t)(slash - path);
  if (n >= cap) n = cap - 1;
  memcpy(buf, path, n);
  buf[n] = 0;
  return buf;
}

/* The inverse, for writing a layout back out: a path under `base` becomes what
 * it is relative to it, and anything else is left absolute. Left absolute
 * rather than climbed out of with `../..`, because a pane whose directory is
 * outside the project is not a fact about the project and a file full of `..`
 * is a file nobody can move. Returns a pointer into `path` or a literal, so
 * there is nothing to free and nothing to size. */
const char *path_relative(const char *path, const char *base) {
  if (!path || !base || !*base) return path;
  size_t n = strlen(base);
  while (n && base[n - 1] == '/') n--; /* a trailing slash is not a component */
  if (strncmp(path, base, n) != 0) return path;
  if (path[n] == 0) return ".";
  if (path[n] != '/') return path; /* /dev/apiary is not under /dev/api */
  const char *rest = path + n + 1;
  return *rest ? rest : ".";
}

/* Create a directory and everything above it. `mkdir` of one level is enough
 * on a machine where ~/.config already exists, which is most of them and not
 * all of them -- a fresh container has neither, and "could not write your
 * config" is a poor first impression. Existing directories are not an error. */
bool path_mkdirs(const char *dir) {
  if (!dir || !*dir) return false;
  char buf[1024];
  snprintf(buf, sizeof buf, "%s", dir);
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/') continue;
    *p = 0;
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
    *p = '/';
  }
  return mkdir(buf, 0700) == 0 || errno == EEXIST;
}
