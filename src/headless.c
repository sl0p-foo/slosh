/* The headless driver (DESIGN.md, "headless from day one").
 *
 * The whole compositor with no tty: commands in on stdin, screen snapshots out
 * on stdout. Tests drive this instead of screen-scraping a pty, so they are
 * deterministic and take milliseconds.
 *
 * The command set is deliberately the vocabulary the M2 control socket will
 * speak, so the harness moves over unchanged.
 *
 *   send <escaped>    bytes as if typed at the outer terminal (decoded first)
 *   raw <escaped>     bytes straight to the pane's pty, decoder bypassed
 *   settle [ms]       pump the pane until it has been quiet for ms
 *   resize <c> <r>
 *   snapshot [json|text]
 *   quit
 *
 * <escaped> understands \e \n \r \t \\ \xHH, so a test can write \e[1;5A.
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

static void hl_event(const input_event_t *ev, void *ud) {
  app_event((app_t *)ud, ev);
}

static int64_t ms_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* \e \n \r \t \\ \xHH -> bytes */
static size_t unescape(const char *in, uint8_t *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in; *p && n < cap; p++) {
    if (*p != '\\') {
      out[n++] = (uint8_t)*p;
      continue;
    }
    p++;
    switch (*p) {
      case 'e': out[n++] = 0x1b; break;
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case '0': out[n++] = 0; break;
      case '\\': out[n++] = '\\'; break;
      case 'x': {
        int hi = 0, lo = 0;
        sscanf(p + 1, "%1x%1x", &hi, &lo);
        out[n++] = (uint8_t)(hi * 16 + lo);
        p += 2;
        break;
      }
      case 0: return n;
      default: out[n++] = (uint8_t)*p; break;
    }
  }
  return n;
}

/* Pump every pane until none has produced anything for `quiet` ms. */
static void settle(app_t *a, int quiet) {
  int64_t last = ms_now();
  int64_t deadline = last + 3000;
  while (!app_should_quit(a) && ms_now() < deadline) {
    int fds[64];
    size_t n = app_fds(a, fds, 64);
    if (n == 0) break;
    struct pollfd pfds[64];
    for (size_t i = 0; i < n; i++) {
      pfds[i].fd = fds[i];
      pfds[i].events = POLLIN;
      pfds[i].revents = 0;
    }
    int r = poll(pfds, (nfds_t)n, 10);
    if (r > 0) {
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
                 int idle_ms, bool script) {
  app_t *a = app_new(argv, cols, rows);
  if (!a) {
    fprintf(stderr, "sl0ptty: cannot spawn pane\n");
    return 1;
  }
  app_resize(a, cols, rows);
  screen_t s;
  screen_init(&s, cols, rows);
  input_parser_t *in = input_new();

  /* one-shot: run it, let it settle, print the screen */
  if (!script) {
    settle(a, idle_ms);
    app_compose(a, &s);
    char *dump = screen_dump(&s);
    fputs(dump, stdout);
    free(dump);
    goto done;
  }

  char line[4096];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    char *sp = strchr(line, ' ');
    const char *arg = sp ? sp + 1 : "";
    if (sp) *sp = 0;

    if (strcmp(line, "send") == 0) {
      uint8_t buf[4096];
      size_t n = unescape(arg, buf, sizeof buf);
      input_feed(in, buf, n, hl_event, a);
      if (input_pending(in)) input_timeout(in, hl_event, a);
    } else if (strcmp(line, "raw") == 0) {
      uint8_t buf[4096];
      size_t n = unescape(arg, buf, sizeof buf);
      app_write_focused(a, buf, n);
    } else if (strcmp(line, "settle") == 0) {
      settle(a, *arg ? atoi(arg) : idle_ms);
    } else if (strcmp(line, "resize") == 0) {
      int c = cols, r = rows;
      sscanf(arg, "%d %d", &c, &r);
      cols = (uint16_t)c;
      rows = (uint16_t)r;
      screen_resize(&s, cols, rows);
      app_resize(a, cols, rows);
    } else if (strcmp(line, "snapshot") == 0) {
      app_compose(a, &s);
      char *dump = strcmp(arg, "text") == 0 ? screen_dump(&s)
                                            : screen_dump_json(&s);
      fputs(dump, stdout);
      fputs("\n", stdout);
      free(dump);
      fflush(stdout);
    } else if (strcmp(line, "panes") == 0) {
      app_compose(a, &s); /* layout is a function of the frame; compose first */
      char *dump = app_panes_json(a);
      fputs(dump, stdout);
      fputs("\n", stdout);
      free(dump);
      fflush(stdout);
    } else if (strcmp(line, "alive") == 0) {
      printf("%s\n", app_should_quit(a) ? "false" : "true");
      fflush(stdout);
    } else if (strcmp(line, "quit") == 0) {
      break;
    } else if (line[0]) {
      fprintf(stderr, "sl0ptty: unknown command: %s\n", line);
    }
  }

done:
  input_free(in);
  screen_free(&s);
  app_free(a);
  return 0;
}
