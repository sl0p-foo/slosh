/* Entry point and argument parsing.
 *
 *   sl0ptty                  attach to session "main", creating it if needed
 *   sl0ptty -s NAME          ... a named session
 *   sl0ptty ls               live sessions
 *   sl0ptty cmd LINE         one control command against a session
 *   sl0ptty --server NAME    run a session in the foreground (internal)
 *   sl0ptty --script         the headless driver (no server, no tty)
 *   sl0ptty --headless       run once, print the screen
 */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "proto.h"
#include "server.h"

int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                 int idle_ms, bool script);

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

static void usage(void) {
  fputs("usage: sl0ptty [-s NAME] [ls | cmd LINE | -- CMD...]\n", stderr);
}

int main(int argc, char **argv) {
  bool headless = false, script = false, server = false;
  uint16_t cols = 80, rows = 24;
  int idle_ms = 300;
  const char *name = "main";
  const char *cmd_line = NULL;
  bool list = false;
  const char *cmd_argv[64];
  int cmd_n = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (strcmp(a, "--headless") == 0) headless = true;
    else if (strcmp(a, "--script") == 0) headless = script = true;
    else if (strcmp(a, "--server") == 0) server = true;
    else if (strcmp(a, "-s") == 0 && i + 1 < argc) name = argv[++i];
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
      fprintf(stderr, "sl0ptty: unknown argument: %s\n", a);
      usage();
      return 2;
    }
  }

  if (cmd_n == 0) {
    const char *shell = getenv("SHELL");
    cmd_argv[cmd_n++] = shell && *shell ? shell : "/bin/sh";
  }
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
      fprintf(stderr, "sl0ptty: no session named %s\n", name);
      return 1;
    }
    return client_control(fd, cmd_line);
  }

  if (headless) return run_headless(cmd_argv, cols, rows, idle_ms, script);

  if (isatty(STDOUT_FILENO)) term_size(&cols, &rows);
  if (server) return server_run(name, cmd_argv, cols, rows);

  /* attach, creating the session if nobody is home */
  int fd = server_connect(name);
  if (fd < 0) fd = server_spawn(name, cmd_argv, cols, rows);
  if (fd < 0) {
    fprintf(stderr, "sl0ptty: cannot start session %s\n", name);
    return 1;
  }
  return client_run(fd);
}
