/* M0: one pty, one fullscreen pane, input passthrough, resize.
 * Plus a seed of the headless harness (M0.5) so this is testable without a tty.
 */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FRAME_MS 8 /* ~120Hz cap; pty reads are coalesced into one paint */

static volatile sig_atomic_t g_winch = 0, g_quit = 0;
static struct termios g_saved_tio;
static bool g_tio_saved = false;

static void on_signal(int sig) {
  if (sig == SIGWINCH) g_winch = 1;
  else g_quit = 1;
}

static void restore_tty(void) {
  if (g_tio_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
  const char *leave = "\x1b[?25h\x1b[0m\x1b[?1049l";
  ssize_t r = write(STDOUT_FILENO, leave, strlen(leave));
  (void)r;
}

static int raw_mode(void) {
  if (tcgetattr(STDIN_FILENO, &g_saved_tio) != 0) return -1;
  g_tio_saved = true;
  struct termios t = g_saved_tio;
  cfmakeraw(&t);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;
  atexit(restore_tty);
  const char *enter = "\x1b[?1049h\x1b[0m\x1b[2J";
  ssize_t r = write(STDOUT_FILENO, enter, strlen(enter));
  (void)r;
  return 0;
}

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

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- headless: run a command, settle, dump the composited screen -------- */

static int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                        int idle_ms) {
  pane_t *p = pane_new(argv, cols, rows, NULL);
  if (!p) {
    fprintf(stderr, "sl0ptty: cannot spawn pane\n");
    return 1;
  }
  screen_t s;
  screen_init(&s, cols, rows);

  int64_t last_activity = now_ms();
  while (pane_alive(p)) {
    struct pollfd pfd = {.fd = pane_fd(p), .events = POLLIN};
    int n = poll(&pfd, 1, 20);
    if (n > 0) {
      if (pane_pump(p) > 0) last_activity = now_ms();
      if (!pane_alive(p)) break;
    }
    if (now_ms() - last_activity > idle_ms) break;
  }
  pane_pump(p);

  pane_compose(p, &s, 0, 0, true);
  char *dump = screen_dump(&s);
  fputs(dump, stdout);
  free(dump);

  screen_free(&s);
  pane_free(p);
  return 0;
}

/* ---- interactive -------------------------------------------------------- */

static int run_interactive(const char *const argv[]) {
  uint16_t cols, rows;
  term_size(&cols, &rows);

  if (raw_mode() != 0) {
    fprintf(stderr, "sl0ptty: stdin is not a terminal (try --headless)\n");
    return 1;
  }

  pane_t *p = pane_new(argv, cols, rows, NULL);
  if (!p) {
    fprintf(stderr, "sl0ptty: cannot spawn pane\n");
    return 1;
  }

  screen_t s;
  screen_init(&s, cols, rows);

  bool pending_paint = true;
  int64_t next_frame = 0;
  bool prefix = false; /* M0 scaffolding: C-a q quits, C-a C-a sends C-a */

  while (!g_quit && pane_alive(p)) {
    int timeout = -1;
    if (pending_paint) {
      int64_t due = next_frame - now_ms();
      timeout = due <= 0 ? 0 : (int)due;
    }

    struct pollfd pfds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = pane_fd(p), .events = POLLIN},
    };
    int n = poll(pfds, 2, timeout);

    if (g_winch) {
      g_winch = 0;
      term_size(&cols, &rows);
      screen_resize(&s, cols, rows);
      pane_resize(p, cols, rows);
      pending_paint = true;
      next_frame = now_ms();
    }

    if (n < 0 && errno != EINTR) break;

    if (n > 0 && (pfds[0].revents & POLLIN)) {
      char buf[8192];
      ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
      if (r > 0) {
        for (ssize_t i = 0; i < r; i++) {
          if (prefix) {
            prefix = false;
            if (buf[i] == 'q') { g_quit = 1; break; }
            if (buf[i] == 0x01) { pane_write(p, "\x01", 1); continue; }
            continue; /* unknown prefix command: swallow */
          }
          if (buf[i] == 0x01) { prefix = true; continue; }
          pane_write(p, &buf[i], 1);
        }
      }
    }

    if (n > 0 && (pfds[1].revents & (POLLIN | POLLHUP))) {
      pane_pump(p);
      if (!pending_paint) {
        pending_paint = true;
        next_frame = now_ms() + FRAME_MS;
      }
    }

    if (pending_paint && now_ms() >= next_frame) {
      pane_compose(p, &s, 0, 0, true);
      screen_flush(&s, STDOUT_FILENO);
      pending_paint = false;
    }
  }

  screen_free(&s);
  pane_free(p);
  restore_tty();
  return 0;
}

int main(int argc, char **argv) {
  bool headless = false;
  uint16_t cols = 80, rows = 24;
  int idle_ms = 300;
  const char *cmd_argv[64];
  int cmd_n = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0) headless = true;
    else if (strcmp(argv[i], "--cols") == 0 && i + 1 < argc) cols = (uint16_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--rows") == 0 && i + 1 < argc) rows = (uint16_t)atoi(argv[++i]);
    else if (strcmp(argv[i], "--idle-ms") == 0 && i + 1 < argc) idle_ms = atoi(argv[++i]);
    else if (strcmp(argv[i], "--") == 0) {
      for (int j = i + 1; j < argc && cmd_n < 63; j++) cmd_argv[cmd_n++] = argv[j];
      break;
    } else {
      fprintf(stderr, "sl0ptty: unknown argument: %s\n", argv[i]);
      return 2;
    }
  }

  if (cmd_n == 0) {
    const char *shell = getenv("SHELL");
    cmd_argv[cmd_n++] = shell && *shell ? shell : "/bin/sh";
  }
  cmd_argv[cmd_n] = NULL;

  struct sigaction sa = {.sa_handler = on_signal};
  sigaction(SIGWINCH, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN); /* no zombies; EOF on the pty is our signal */

  return headless ? run_headless(cmd_argv, cols, rows, idle_ms)
                  : run_interactive(cmd_argv);
}
