/* Entry point and argument parsing.
 *
 *   sl0ppty                  attach to session "main", creating it if needed
 *   sl0ppty -s NAME          ... a named session
 *   sl0ppty ls               live sessions
 *   sl0ppty cmd LINE         one control command against a session
 *   sl0ppty --server NAME    run a session in the foreground (internal)
 *   sl0ppty --script         the headless driver (no server, no tty)
 *   sl0ppty --headless       run once, print the screen
 */
#define _GNU_SOURCE
#include "sl0ppty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "proto.h"
#include "server.h"
#include "config.h"
#include "png.h"
#include "version.h"

int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                 int idle_ms, bool script, const char *layout);

static void term_size(uint16_t *cols, uint16_t *rows) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
  } else {
    *cols = 80;
    *rows = 24;
  }
}

/* `sl0ppty --check [FILE]`: read a config the way a session would and say what
 * it could not honour, one problem per line, `file:line: what`. Exits 1 when
 * there is anything to say, so it drops into an editor's compile step or a
 * pre-commit hook without any glue.
 *
 * Worth being a mode rather than a script: the only checker that cannot drift
 * from the loader is the loader, and the difference between this and a session
 * is that a session shows the first complaint (it has one status line) while
 * this shows all of them. */
static int check_config(const char *path) {
  config_t cfg;
  config_defaults(&cfg);
  char err[256] = {0};
  const char *file = path ? path : config_default_path();
  bool parsed = config_load(&cfg, file, err, sizeof err);

  const char *msgs[CONFIG_MSGS_MAX];
  size_t n = config_messages(&cfg, msgs, CONFIG_MSGS_MAX);
  for (size_t i = 0; i < n; i++) fprintf(stderr, "  %s\n", msgs[i]);

  if (!parsed) {
    /* The file could not be read or parsed at all, so nothing in it applied --
     * a different thing from a line that was skipped, and worth saying so. */
    fprintf(stderr, "%s: not loaded (defaults would stand)\n", file);
    config_free(&cfg);
    return 1;
  }

  /* What it did read, so a clean run is evidence rather than silence: an empty
   * answer and a file that was never opened look identical otherwise. */
  const char *files[CONFIG_FILES_MAX];
  size_t nf = config_files(&cfg, files, CONFIG_FILES_MAX);
  if (n) {
    fprintf(stderr, "%s: %zu problem%s\n", file, n, n == 1 ? "" : "s");
  } else {
    char prefix[24];
    config_chord_name(cfg.prefix_key, cfg.prefix_mods, prefix, sizeof prefix);
    printf("%s: ok\n", file);
    for (size_t i = 0; i < nf; i++)
      printf("  read     %s\n", files[i]);
    printf("  prefix   %s\n", prefix);
    printf("  bindings %zu\n", cfg.nbinds);
  }
  config_free(&cfg);
  return n ? 1 : 0;
}

static void usage(void) {
  fputs("usage: sl0ppty [-s NAME] [--layout FILE] [--no-reload]\n"
        "                [--version] [--dump-config] [--check [FILE]]\n"
        "                [ls | cmd LINE | -- CMD...]\n",
        stderr);
}

int main(int argc, char **argv) {
  /* Process-global and required before any terminal exists, so it happens
   * here rather than in whichever mode we turn out to be: server, headless
   * and the script driver all create panes, and a pane created without this
   * silently drops every PNG a program sends it. */
  png_init();

  bool headless = false, script = false, server = false;
  uint16_t cols = 80, rows = 24;
  int idle_ms = 300;
  const char *name = "main";
  const char *cmd_line = NULL;
  const char *layout = NULL;
  bool watch = true;
  bool list = false;
  const char *cmd_argv[64];
  int cmd_n = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--headless") == 0) headless = true;
    else if (strcmp(a, "--script") == 0) headless = script = true;
    else if (strcmp(a, "--server") == 0) server = true;
    else if (strcmp(a, "-s") == 0 && i + 1 < argc) name = argv[++i];
    else if (strcmp(a, "--layout") == 0 && i + 1 < argc) layout = argv[++i];
    else if (strcmp(a, "--no-reload") == 0) watch = false;
    else if (strcmp(a, "--check") == 0) {
      /* An optional path, so it lints a file you have not installed yet:
       * `sl0ppty --check theme.kdl`. Without one it checks the config a session
       * would actually read. */
      const char *path = (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : NULL;
      return check_config(path);
    }
    else if (strcmp(a, "--dump-config") == 0) {
      /* Every setting with the value it currently has, as a file you could
       * have written. `sl0ppty --dump-config > ~/.config/sl0ppty/config.kdl`
       * is a supported way to start one. */
      char *text = config_dump_defaults();
      fputs(text, stdout);
      free(text);
      return 0;
    }
    else if (strcmp(a, "--version") == 0) {
      /* The same string the status line shows, so "which build is this" has
       * one answer whether you ask the binary or the session. */
      printf("sl0ppty %s\n", SL0PPTY_VERSION);
      return 0;
    }
    else if (strcmp(a, "ls") == 0) list = true;
    else if (strcmp(a, "cmd") == 0 && i + 1 < argc) cmd_line = argv[++i];
    else if (strcmp(a, "--cols") == 0 && i + 1 < argc) cols = (uint16_t)atoi(argv[++i]);
    else if (strcmp(a, "--rows") == 0 && i + 1 < argc) rows = (uint16_t)atoi(argv[++i]);
    else if (strcmp(a, "--idle-ms") == 0 && i + 1 < argc) idle_ms = atoi(argv[++i]);
    else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage();
      return 0;
    } else if (strcmp(a, "--") == 0) {
      for (int j = i + 1; j < argc && cmd_n < 63; j++) cmd_argv[cmd_n++] = argv[j];
      break;
    } else {
      fprintf(stderr, "sl0ppty: unknown argument: %s\n", a);
      usage();
      return 2;
    }
  }

  /* No `--` command: leave it empty and let the session decide, so the
   * config's `shell` is consulted when a pane is made rather than baked in
   * here -- where it was not consulted at all, which made `shell` a setting
   * that did nothing. */
  cmd_argv[cmd_n] = NULL;

  if (list) {
    char **names = NULL;
    size_t n = session_list(&names);
    for (size_t i = 0; i < n; i++) {
      int fd = server_connect(names[i]);
      printf("%-20s %s\n", names[i], fd >= 0 ? "running" : "stale");
      if (fd >= 0) close(fd);
      free(names[i]);
    }
    free(names);
    return 0;
  }

  if (cmd_line) {
    int fd = server_connect(name);
    if (fd < 0) {
      fprintf(stderr, "sl0ppty: no session named %s\n", name);
      return 1;
    }
    return client_control(fd, cmd_line);
  }

  if (headless) return run_headless(cmd_argv, cols, rows, idle_ms, script, layout);

  if (isatty(STDOUT_FILENO)) term_size(&cols, &rows);
  if (server) return server_run(name, cmd_argv, cols, rows, layout, watch);

  /* attach, creating the session if nobody is home */
  int fd = server_connect(name);
  if (fd < 0) fd = server_spawn(name, cmd_argv, cols, rows, layout, watch);
  if (fd < 0) {
    fprintf(stderr, "sl0ppty: cannot start session %s\n", name);
    return 1;
  }
  return client_run(fd);
}
