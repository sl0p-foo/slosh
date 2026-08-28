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

/* Whether a path names a place outright, rather than one to be read against
 * somewhere else.
 *
 * On POSIX that is a leading `/` and nothing else. Windows spells it three
 * ways and all three have to count: rooted on the current drive (`\dir`),
 * rooted on a named one (`C:\dir`, `C:/dir`), and a UNC share
 * (`\\server\share`). Getting it wrong is not a lookup that fails, it is a
 * concatenation that succeeds at being nonsense -- `include "C:/themes/t.kdl"`
 * was resolved against the including file's directory and opened as
 * `C:/Users/you/.config/slosh/C:/themes/t.kdl`.
 *
 * A drive letter must be followed by a separator to be absolute: `C:file` is
 * relative to the current directory *of drive C*, which is a different thing
 * and not one we can resolve for anybody.
 *
 * All of it is Windows-only on purpose. `C:` is an ordinary file name on
 * POSIX and a path that begins with one is relative there, so reading it as a
 * drive would break the platform that works to fix the one that does not. */
bool path_is_absolute(const char *path) {
  if (!path || !*path) return false;
  if (path[0] == '/') return true;
#ifdef _WIN32
  if (path[0] == '\\') return true;
  bool drive =
      (path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z');
  if (drive && path[1] == ':' && (path[2] == '/' || path[2] == '\\'))
    return true;
#endif
  return false;
}

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
  if (path_is_absolute(path) || !base || !*base) return path;
  /* `.` is the project directory itself, which is what a dump writes for the
   * pane that started there -- joined naively it would be `/home/you/dev/api/.`,
   * which works and reads like a bug. */
  if (strcmp(path, ".") == 0) return base;
  if (strncmp(path, "./", 2) == 0) path += 2;
  snprintf(buf, cap, "%s/%s", base, path);
  return buf;
}

/* The last separator in a path, whichever kind it is.
 *
 * Windows takes both and mixes them without being asked: slosh joins with '/'
 * and the environment supplies '\\', so `$HOME` plus a literal lands as
 * `C:\Users\you/.config/slosh/config.kdl`. A path that has been near a native
 * API comes back all backslashes, and `SLOSH_CONFIG=C:\Users\you\slosh.kdl`
 * is how a person would write it in the first place.
 *
 * Windows-only, because a backslash is a legal character in a POSIX file
 * name: splitting on it there would invent a directory out of a file called
 * `a\b`. */
static const char *last_sep(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *back = strrchr(path, '\\');
  if (back && (!slash || back > slash)) slash = back;
#endif
  return slash;
}

/* The directory a file lives in: what `path_resolve` wants as its base. A path
 * with no separator in it is in the current one, which is `.` rather than `""`
 * -- an empty base means "no base" to everything downstream. */
const char *path_dir(const char *path, char *buf, size_t cap) {
  if (!path) return NULL;
  const char *slash = last_sep(path);
  if (!slash) return ".";
  if (slash == path) return *slash == '/' ? "/" : "\\";
  size_t n = (size_t)(slash - path);
#ifdef _WIN32
  /* `C:\config.kdl` lives in `C:\`, not in `C:` -- which names the current
   * directory *on* drive C and is a different place. Keep the separator.
   * Windows-only: on POSIX `a:` is just a directory called `a:`. */
  if (n == 2 && path[1] == ':') n = 3;
#endif
  if (n >= cap) n = cap - 1;
  memcpy(buf, path, n);
  buf[n] = 0;
  return buf;
}

/* The name part, the other half of path_dir. Points into `path`, so there is
 * nothing to size and nothing to free. */
const char *path_base(const char *path) {
  if (!path) return NULL;
  const char *slash = last_sep(path);
  return slash ? slash + 1 : path;
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
