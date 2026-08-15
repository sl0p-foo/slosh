/* Paths as people write them.
 *
 * `~` is a shell thing: by the time a path reaches us from a config or layout
 * file no shell has touched it, so `cwd="~/dev/api"` was being handed to
 * chdir() literally, failing, and leaving the pane in whatever directory the
 * session happened to be in. Silently \u2014 which is the worst way for a path to
 * be wrong, because the pane still starts and still works.
 */
#include "sl0ppty.h"

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
