/* Clicking a path open: the pure half.
 *
 * Everything here is text in, text out, plus stat(): no terminal, no panes, no
 * config. That split is deliberate -- the interesting decisions (where a path
 * starts and stops, what `:12:5` means, which directory a relative name was
 * relative to, how a given editor spells "go to line 12") are exactly the ones
 * worth a unit test, and none of them need a session to answer.
 *
 * The app half (app_open_path) supplies the line under the pointer, the
 * directories a relative path could have meant, and the pane to run the editor
 * in. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define OPENPATH_MAX 4096

typedef struct {
  char path[OPENPATH_MAX]; /* as written: maybe relative, maybe ~/... */
  long line, col;          /* 0 when the text did not say */
  /* What openpath_scan matched, as byte offsets into its text: the path and
   * any `:line:col` stuck to it, without the punctuation around it. This is
   * what a hover underlines. */
  size_t start, end;
} openpath_ref_t;

/* The path-shaped token covering byte `at` of `text` (one logical line, soft
 * wraps already joined), with any `:line:col`, `(line,col)` or Python
 * `", line N"` that came with it. False when `at` is on whitespace or a
 * delimiter, or the token is plainly not a path (a URL). Says nothing about
 * whether the file exists: that is openpath_resolve's question. */
bool openpath_scan(const char *text, size_t len, size_t at,
                   openpath_ref_t *out);

/* A `file://` URI (an OSC 8 link) as a local path, percent-decoded, with a
 * trailing `#L12` or `:12` taken as the line. `hostname` is this machine's
 * name: a link naming any *other* host is somebody else's file and is refused,
 * which is what keeps a path printed over ssh from opening the wrong file.
 * An empty host and `localhost` are always this machine. */
bool openpath_from_uri(const char *uri, size_t len, const char *hostname,
                       openpath_ref_t *out);

/* An existing file or directory for `path`: `~` expanded, an absolute path
 * taken as it is, a relative one tried against each of `dirs` in order --
 * and, failing that, without a git-diff `a/` or `b/` prefix. False when
 * nothing exists, which is the whole safety property: a word that merely looks
 * like a path is not a path. */
bool openpath_resolve(const char *path, const char *const *dirs, size_t ndirs,
                      char *out, size_t cap);

/* The repository root above `dir` (the nearest directory holding `.git`), or
 * false. Diffs and `make -C` print paths relative to the root, not to where
 * the shell happens to be. */
bool openpath_git_root(const char *dir, char *out, size_t cap);

/* The shell command that opens `path` at `line`/`col` (0: not given).
 *
 * `tmpl`, when set, is the user's own command: `{path}` and `{dir}` become
 * shell-quoted, `{line}` and `{col}` become numbers (1 when not given), and a
 * template that never says `{path}` gets it appended. Without one, `editor`
 * is run with the line in whatever spelling it understands -- `+12` for the vi
 * family, `path:12:5` for helix, `--goto` for VS Code -- and with just the
 * path for an editor this does not know. */
bool openpath_command(const char *tmpl, const char *editor, const char *path,
                      long line, long col, char *out, size_t cap);
