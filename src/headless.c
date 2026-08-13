/* The headless driver (DESIGN.md, "headless from day one").
 *
 * The whole compositor with no tty: commands in on stdin, replies out on
 * stdout. The vocabulary lives in cmd.c and is shared with the control socket,
 * so a script written against one works against the other.
 *
 * `settle` is implemented here because it needs an event loop.
 */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "cmd.h"

#define MAX_PANES 64

static int64_t ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Pump every pane until none has produced anything for `quiet` ms. */
static void settle(app_t *a, int quiet) {
  int64_t last = ms_now();
  int64_t deadline = last + 3000;
  while (!app_should_quit(a) && ms_now() < deadline) {
    int fds[MAX_PANES];
    size_t n = app_fds(a, fds, MAX_PANES);
    if (n == 0) break;
    struct pollfd pfds[MAX_PANES];
    for (size_t i = 0; i < n; i++)
      pfds[i] = (struct pollfd){.fd = fds[i], .events = POLLIN};
    if (poll(pfds, (nfds_t)n, 10) > 0) {
      for (size_t i = 0; i < n; i++)
        if (pfds[i].revents) {
          app_pump_fd(a, pfds[i].fd);
          last = ms_now();
        }
    }
    app_reap(a);
    if (ms_now() - last >= quiet) break;
  }
}

int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                 int idle_ms, bool script, const char *layout) {
  app_t *a = app_new(argv, cols, rows);
  if (!a) {
    fprintf(stderr, "sl0ptty: cannot spawn pane\n");
    return 1;
  }
  if (layout) {
    char err[256] = {0};
    if (!app_apply_layout_file(a, layout, true, err, sizeof err))
      fprintf(stderr, "sl0ptty: %s: %s\n", layout, err[0] ? err : "bad layout");
  }
  app_resize(a, cols, rows);
  screen_t s;
  screen_init(&s, cols, rows);
  input_parser_t *in = input_new();

  if (!script) { /* one-shot: run it, let it settle, print the screen */
    settle(a, idle_ms);
    app_compose(a, &s);
    char *dump = screen_dump(&s);
    fputs(dump, stdout);
    free(dump);
    goto done;
  }

  /* Paint once before accepting input: a click resolves against what was
   * painted, and in a real session a frame always precedes the pointer. */
  app_compose(a, &s);

  char line[4096];
  bool quit = false;
  while (!quit && fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!line[0]) continue;

    if (strncmp(line, "settle", 6) == 0) {
      settle(a, line[6] == ' ' ? atoi(line + 7) : idle_ms);
      continue;
    }

    char *reply = cmd_exec(a, &s, in, line, &quit);
    if (!reply) {
      fprintf(stderr, "sl0ptty: unknown command: %s\n", line);
      continue;
    }
    /* Only commands that answer print a line, so the harness stays in step. */
    if (*reply) {
      fputs(reply, stdout);
      fputc('\n', stdout);
      fflush(stdout);
    }
    free(reply);
  }

done:
  input_free(in);
  screen_free(&s);
  app_free(a);
  return 0;
}
