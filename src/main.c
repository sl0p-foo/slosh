/* M0: one pty, one fullscreen pane, input passthrough, resize.
 * Plus a seed of the headless harness (M0.5) so this is testable without a tty.
 */
#define _GNU_SOURCE
#include "sl0ptty.h"

#include <ghostty/vt.h>

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

#define FRAME_MS 8   /* ~120Hz cap; pty reads are coalesced into one paint */
#define ESC_MS 40    /* how long a bare ESC waits to become a sequence */

/* What we ask the outer terminal for. Kitty keyboard flag 1 (disambiguate)
 * costs nothing on terminals that ignore it, and everything a pane receives is
 * re-encoded for that pane anyway, so an inner app that has never heard of
 * kitty still gets legacy bytes. */
#define TERM_ENTER \
  "\x1b[?1049h\x1b[0m\x1b[2J" /* alt screen */ \
  "\x1b[?1002h\x1b[?1006h"    /* mouse: button+drag, SGR encoding */ \
  "\x1b[?1004h"               /* focus in/out */ \
  "\x1b[?2004h"               /* bracketed paste */ \
  "\x1b[>1u"                  /* kitty keyboard, pushed */
#define TERM_LEAVE \
  "\x1b[<u\x1b[?2004l\x1b[?1004l\x1b[?1006l\x1b[?1002l" \
  "\x1b[?25h\x1b[0m\x1b[?1049l"

static volatile sig_atomic_t g_winch = 0, g_quit = 0;
static struct termios g_saved_tio;
static bool g_tio_saved = false;

static void on_signal(int sig) {
  if (sig == SIGWINCH) g_winch = 1;
  else g_quit = 1;
}

static void restore_tty(void) {
  if (g_tio_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_tio);
  ssize_t r = write(STDOUT_FILENO, TERM_LEAVE, strlen(TERM_LEAVE));
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
  ssize_t r = write(STDOUT_FILENO, TERM_ENTER, strlen(TERM_ENTER));
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

/* headless lives in headless.c */
int run_headless(const char *const argv[], uint16_t cols, uint16_t rows,
                 int idle_ms, bool script);

/* ---- interactive -------------------------------------------------------- */

/* Where decoded events go. The prefix (C-a) is handled here, on semantic
 * events, so it cannot be confused by a byte that merely looks like C-a in the
 * middle of a UTF-8 sequence or an escape. */
typedef struct {
  pane_t *pane;
  bool prefix;
  volatile sig_atomic_t *quit;
} dispatch_t;

static void on_event(const input_event_t *ev, void *ud) {
  dispatch_t *d = ud;

  if (ev->kind == EV_KEY && ev->action != KEY_RELEASE) {
    bool ctrl_a = (ev->mods & MOD_CTRL) && ev->unshifted == 'a';
    if (d->prefix) {
      d->prefix = false;
      if (ev->key == GHOSTTY_KEY_Q) { *d->quit = 1; return; }
      if (ctrl_a) { pane_send_key(d->pane, ev); return; } /* C-a C-a = literal */
      return;                                             /* unbound: swallow */
    }
    if (ctrl_a) {
      d->prefix = true;
      return;
    }
  }

  switch (ev->kind) {
    case EV_KEY: pane_send_key(d->pane, ev); break;
    case EV_MOUSE: pane_send_mouse(d->pane, ev); break;
    case EV_PASTE: pane_send_paste(d->pane, ev->paste, ev->paste_len); break;
    case EV_FOCUS: break; /* M1: focus follows the layout, not the client */
    default: break;
  }
}

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

  input_parser_t *in = input_new();
  dispatch_t d = {.pane = p, .quit = &g_quit};

  bool pending_paint = true;
  int64_t next_frame = 0;
  int64_t esc_due = 0;

  while (!g_quit && pane_alive(p)) {
    int timeout = -1;
    if (pending_paint) {
      int64_t due = next_frame - now_ms();
      timeout = due <= 0 ? 0 : (int)due;
    }
    if (input_pending(in)) {
      int64_t due = esc_due - now_ms();
      int t = due <= 0 ? 0 : (int)due;
      if (timeout < 0 || t < timeout) timeout = t;
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
      uint8_t buf[8192];
      ssize_t r = read(STDIN_FILENO, buf, sizeof buf);
      if (r > 0) {
        input_feed(in, buf, (size_t)r, on_event, &d);
        esc_due = now_ms() + ESC_MS;
      }
    }

    /* A lone ESC is only the Escape key once nothing follows it. */
    if (input_pending(in) && now_ms() >= esc_due) input_timeout(in, on_event, &d);

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

  input_free(in);
  screen_free(&s);
  pane_free(p);
  restore_tty();
  return 0;
}

int main(int argc, char **argv) {
  bool headless = false, script = false;
  uint16_t cols = 80, rows = 24;
  int idle_ms = 300;
  const char *cmd_argv[64];
  int cmd_n = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--headless") == 0) headless = true;
    else if (strcmp(argv[i], "--script") == 0) headless = script = true;
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

  return headless ? run_headless(cmd_argv, cols, rows, idle_ms, script)
                  : run_interactive(cmd_argv);
}
