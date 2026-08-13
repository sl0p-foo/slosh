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

typedef struct {
  pane_t *pane;
} hl_dispatch_t;

static void hl_event(const input_event_t *ev, void *ud) {
  hl_dispatch_t *d = ud;
  switch (ev->kind) {
    case EV_KEY: pane_send_key(d->pane, ev); break;
    case EV_MOUSE: pane_send_mouse(d->pane, ev); break;
    case EV_PASTE: pane_send_paste(d->pane, ev->paste, ev->paste_len); break;
    default: break;
  }
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

/* Pump the pane until it has produced nothing for `quiet` ms. */
static void settle(pane_t *p, int quiet) {
  int64_t last = ms_now();
  int64_t deadline = last + 3000;
  while (pane_alive(p) && ms_now() < deadline) {
    struct pollfd pfd = {.fd = pane_fd(p), .events = POLLIN};
    int n = poll(&pfd, 1, 10);
    if (n > 0) {
      if (pane_pump(p) > 0) last = ms_now();
      if (!pane_alive(p)) break;
    }
    if (ms_now() - last >= quiet) break;
  }
}

int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                 int idle_ms, bool script) {
  pane_t *p = pane_new(argv, cols, rows, NULL);
  if (!p) {
    fprintf(stderr, "sl0ptty: cannot spawn pane\n");
    return 1;
  }
  screen_t s;
  screen_init(&s, cols, rows);
  input_parser_t *in = input_new();
  hl_dispatch_t d = {.pane = p};

  /* one-shot: run it, let it settle, print the screen */
  if (!script) {
    settle(p, idle_ms);
    pane_compose(p, &s, 0, 0, true);
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
      input_feed(in, buf, n, hl_event, &d);
      if (input_pending(in)) input_timeout(in, hl_event, &d);
    } else if (strcmp(line, "raw") == 0) {
      uint8_t buf[4096];
      size_t n = unescape(arg, buf, sizeof buf);
      pane_write(p, buf, n);
    } else if (strcmp(line, "settle") == 0) {
      settle(p, *arg ? atoi(arg) : idle_ms);
    } else if (strcmp(line, "resize") == 0) {
      int c = cols, r = rows;
      sscanf(arg, "%d %d", &c, &r);
      cols = (uint16_t)c;
      rows = (uint16_t)r;
      screen_resize(&s, cols, rows);
      pane_resize(p, cols, rows);
    } else if (strcmp(line, "snapshot") == 0) {
      pane_compose(p, &s, 0, 0, true);
      char *dump = strcmp(arg, "text") == 0 ? screen_dump(&s)
                                            : screen_dump_json(&s);
      fputs(dump, stdout);
      fputs("\n", stdout);
      free(dump);
      fflush(stdout);
    } else if (strcmp(line, "alive") == 0) {
      printf("%s\n", pane_alive(p) ? "true" : "false");
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
  pane_free(p);
  return 0;
}
