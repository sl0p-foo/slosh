/* Entry point and argument parsing.
 *
 *   slosh                  attach to session "main", creating it if needed
 *   slosh -s NAME          ... a named session
 *   slosh ls               live sessions
 *   slosh cmd LINE         one control command against a session
 *   slosh --server NAME    run a session in the foreground (internal)
 *   slosh --script         the headless driver (no server, no tty)
 *   slosh --headless       run once, print the screen
 */
#define _GNU_SOURCE
#include "slosh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "app.h"
#include "proto.h"
#include "server.h"
#include "config.h"
#include "png.h"
#include "version.h"

#ifndef _WIN32
#include <errno.h>
#include <sys/stat.h>

#include "terminfo.h"

/* Write the embedded xterm-ghostty terminfo entry into ~/.terminfo, which
 * is the first place curses looks: no root, no package, no conflict with a
 * ghostty the machine may install later (its own entry in the system
 * database simply stops being needed). Written under every name the entry
 * answers to, in the letter layout and Darwin's hashed one, because the
 * lookup is by filename. */
static int install_terminfo(void) {
  const char *home = getenv("HOME");
  if (!home || !*home) {
    fputs("slosh: no $HOME to install into\n", stderr);
    return 1;
  }
  /* dir, then name inside it: x/ and 78/ for xterm-ghostty, g/ and 67/ for
   * the ghostty alias. */
  static const char *const spots[][2] = {
      {"x", "xterm-ghostty"},
      {"78", "xterm-ghostty"},
      {"g", "ghostty"},
      {"67", "ghostty"},
  };
  char path[1024];
  snprintf(path, sizeof path, "%s/.terminfo", home);
  if (mkdir(path, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "slosh: mkdir %s: %s\n", path, strerror(errno));
    return 1;
  }
  for (size_t i = 0; i < sizeof spots / sizeof *spots; i++) {
    snprintf(path, sizeof path, "%s/.terminfo/%s", home, spots[i][0]);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
      fprintf(stderr, "slosh: mkdir %s: %s\n", path, strerror(errno));
      return 1;
    }
    snprintf(path, sizeof path, "%s/.terminfo/%s/%s", home, spots[i][0],
             spots[i][1]);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(TERMINFO_GHOSTTY, 1, sizeof TERMINFO_GHOSTTY, f) !=
                  sizeof TERMINFO_GHOSTTY) {
      fprintf(stderr, "slosh: write %s: %s\n", path, strerror(errno));
      if (f) fclose(f);
      return 1;
    }
    fclose(f);
  }
  printf("installed xterm-ghostty (and the ghostty alias) into %s/.terminfo\n"
         "new sessions use it; a session already running keeps the TERM it\n"
         "chose at startup until it is restarted\n",
         home);
  return 0;
}
#endif

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

/* Whether a file is a layout rather than a config, decided the same way both
 * loaders decide it: by what its top-level names are. A file that will not
 * parse has no top-level names, and then the extension is all there is -- which
 * is exactly the job D2 gave it, and better than telling somebody their layout
 * is a config whose "defaults would stand". */
static bool looks_like_layout(const char *path) {
  char buf[1024];
  kdl_node_t *root =
      kdl_parse_file(path_expand(path, buf, sizeof buf), NULL, 0);
  if (!root) {
    /* `x.layout` is the spelling, `x.layout.kdl` the one it replaced: both
     * are somebody's layout, whatever it was that would not parse. */
    size_t n = strlen(path), s = strlen(".layout");
    if (n >= s && strcmp(path + n - s, ".layout") == 0) return true;
    return strstr(path, ".layout.") != NULL;
  }
  bool layout = false;
  for (size_t i = 0; i < root->nkids && !layout; i++) {
    const char *name = root->kids[i] ? root->kids[i]->name : NULL;
    if (!name) continue;
    if (strcmp(name, "layout") == 0 || strcmp(name, "tab") == 0) layout = true;
  }
  kdl_free(root);
  return layout;
}

/* `slosh --check FILE` on a layout. Telling somebody they handed a layout to
 * the config checker was honest and unhelpful: the file they want checked is
 * the one they were told to check. Same output shape, same exit status. */
static int check_layout(const char *path) {
  layout_msg_t msgs[LAYOUT_MSGS_MAX];
  size_t dropped = 0;
  size_t n = layout_check_file(path, msgs, LAYOUT_MSGS_MAX, &dropped);
  for (size_t i = 0; i < n; i++) fprintf(stderr, "  %s\n", msgs[i]);
  if (dropped) fprintf(stderr, "  ...and %zu more\n", dropped);
  if (n)
    fprintf(stderr, "%s: %zu problem%s\n", path, n + dropped,
            n + dropped == 1 ? "" : "s");
  else
    printf("%s: ok, a layout\n", path);
  return n ? 1 : 0;
}

/* `slosh --check [FILE]`: read a config the way a session would and say what
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
    for (size_t i = 0; i < nf; i++) printf("  read     %s\n", files[i]);
    printf("  prefix   %s\n", prefix);
    printf("  bindings %zu\n", cfg.nbinds);
  }
  config_free(&cfg);
  return n ? 1 : 0;
}

static void usage(void) {
  fputs("usage: slosh [-s NAME] [--layout FILE] [--no-reload]\n"
        "                [--version] [--dump-config] [--check [FILE]]\n"
        "                [--install-terminfo] [ls | cmd LINE | -- CMD...]\n",
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
    if (strcmp(a, "--headless") == 0)
      headless = true;
    else if (strcmp(a, "--script") == 0)
      headless = script = true;
    else if (strcmp(a, "--server") == 0)
      server = true;
    else if (strcmp(a, "-s") == 0 && i + 1 < argc)
      name = argv[++i];
    else if (strcmp(a, "--layout") == 0 && i + 1 < argc)
      layout = argv[++i];
    else if (strcmp(a, "--no-reload") == 0)
      watch = false;
    else if (strcmp(a, "--check") == 0) {
      /* An optional path, so it lints a file you have not installed yet:
       * `slosh --check theme.kdl`. Without one it checks the config a session
       * would actually read. A layout is checked as a layout: one flag, and the
       * file decides which schema it is held to. */
      const char *path =
          (i + 1 < argc && argv[i + 1][0] != '-') ? argv[++i] : NULL;
      return path && looks_like_layout(path) ? check_layout(path)
                                             : check_config(path);
    } else if (strcmp(a, "--dump-config") == 0) {
      /* Every setting with its *default*, as a file you could have written:
       * `slosh --dump-config > ~/.config/slosh/config.kdl` is a supported
       * way to start one. Deliberately not the values a config in effect gives
       * them -- this is the file to begin from, and `--check` is the one that
       * reads yours and says what it understood. */
      char *text = config_dump_defaults();
      fputs(text, stdout);
      free(text);
      return 0;
    } else if (strcmp(a, "--install-terminfo") == 0) {
#ifdef _WIN32
      fputs("slosh: terminfo is not a Windows concept; nothing to install\n",
            stderr);
      return 1;
#else
      return install_terminfo();
#endif
    } else if (strcmp(a, "--version") == 0) {
      /* The same string the status line shows, so "which build is this" has
       * one answer whether you ask the binary or the session. */
      printf("slosh %s\n", SLOSH_VERSION);
      return 0;
    } else if (strcmp(a, "ls") == 0)
      list = true;
    else if (strcmp(a, "cmd") == 0 && i + 1 < argc)
      cmd_line = argv[++i];
    else if (strcmp(a, "--cols") == 0 && i + 1 < argc)
      cols = (uint16_t)atoi(argv[++i]);
    else if (strcmp(a, "--rows") == 0 && i + 1 < argc)
      rows = (uint16_t)atoi(argv[++i]);
    else if (strcmp(a, "--idle-ms") == 0 && i + 1 < argc)
      idle_ms = atoi(argv[++i]);
    else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage();
      return 0;
    } else if (strcmp(a, "--") == 0) {
      for (int j = i + 1; j < argc && cmd_n < 63; j++)
        cmd_argv[cmd_n++] = argv[j];
      break;
    } else {
      fprintf(stderr, "slosh: unknown argument: %s\n", a);
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
      fprintf(stderr, "slosh: no session named %s\n", name);
      return 1;
    }
    return client_control(fd, cmd_line);
  }

  if (headless)
    return run_headless(cmd_argv, cols, rows, idle_ms, script, layout);

  if (isatty(STDOUT_FILENO)) term_size(&cols, &rows);
  if (server) return server_run(name, cmd_argv, cols, rows, layout, watch);

  /* Not to the session this process is already inside. `slosh` typed into a
   * pane of "main" would attach that pane's terminal back to the session which
   * is drawing it: a recursive view whose own output becomes its next input.
   * SLOSH_SESSION names the pane's own session; asking for any *other* name
   * is deliberate nesting and none of our business. The one legitimate shape
   * this refuses — a fresh terminal launched from a pane, inheriting the
   * variables, deliberately making that recursive attachment — gets its escape hatch
   * named in the message rather than a flag: it is exactly what unsetting
   * the variable means. */
  const char *inside = getenv("SLOSH") ? getenv("SLOSH_SESSION") : NULL;
  if (inside && *inside && strcmp(inside, name) == 0) {
    fprintf(stderr,
            "slosh: this pane is already inside session %s\n"
            "  (C-a d detaches; `slosh -s NAME` nests another session;\n"
            "   unset SLOSH_SESSION to attach recursively anyway)\n",
            name);
    return 1;
  }

  /* attach, creating the session if nobody is home */
  int fd = server_connect(name);
  if (fd < 0) fd = server_spawn(name, cmd_argv, cols, rows, layout, watch);
  if (fd < 0) {
    fprintf(stderr, "slosh: cannot start session %s\n", name);
    return 1;
  }
  return client_run(fd);
}
