/* The client: a terminal in raw mode and a socket. It decodes nothing and
 * lays out nothing — it forwards bytes and paints what it is sent, so a bug
 * here can cost you a frame, never a session. */
#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "proto.h"

#define TERM_ENTER                                                             \
  "\x1b[?1049h\x1b[0m\x1b[2J"                                                  \
  "\x1b[?1002h\x1b[?1003h\x1b[?1006h"                                          \
  "\x1b[?1004h"                                                                \
  "\x1b[?2004h"                                                                \
  "\x1b[>1u"
#define TERM_LEAVE                                                             \
  "\x1b[<u\x1b[?2004l\x1b[?1004l\x1b[?1006l\x1b[?1003l\x1b[?1002l"             \
  "\x1b[?25h\x1b[0m\x1b[?1049l"

static volatile sig_atomic_t g_winch = 0, g_quit = 0;
static struct termios g_saved;
static bool g_saved_ok = false;

static void on_sig(int s) {
  if (s == SIGWINCH)
    g_winch = 1;
  else
    g_quit = 1;
}

static void restore(void) {
  if (g_saved_ok) tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
  ssize_t r = write(STDOUT_FILENO, TERM_LEAVE, strlen(TERM_LEAVE));
  (void)r;
}

/* The terminal's size, in cells and — when it will say — in pixels per cell.
 * Most terminals fill in the pixel fields; the ones that do not report zero,
 * which is passed on as "I do not know" rather than as a size. */
static void term_size(uint16_t *cols, uint16_t *rows, uint16_t *cell_w,
                      uint16_t *cell_h) {
  struct winsize ws;
  *cell_w = *cell_h = 0;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    if (ws.ws_xpixel && ws.ws_ypixel) {
      *cell_w = (uint16_t)(ws.ws_xpixel / ws.ws_col);
      *cell_h = (uint16_t)(ws.ws_ypixel / ws.ws_row);
    }
  } else {
    *cols = 80;
    *rows = 24;
  }
}

static void send_size(int fd, uint8_t type, uint16_t cols, uint16_t rows,
                      uint16_t cell_w, uint16_t cell_h) {
  uint8_t b[8] = {(uint8_t)(cols >> 8),   (uint8_t)cols,
                  (uint8_t)(rows >> 8),   (uint8_t)rows,
                  (uint8_t)(cell_w >> 8), (uint8_t)cell_w,
                  (uint8_t)(cell_h >> 8), (uint8_t)cell_h};
  msg_send(fd, type, b, sizeof b);
}

static void write_all(int fd, const void *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, (const char *)buf + off, len - off);
    if (w <= 0) {
      if (errno == EINTR) continue;
      return;
    }
    off += (size_t)w;
  }
}

int client_run(int fd) {
  uint16_t cols, rows, cell_w, cell_h;
  term_size(&cols, &rows, &cell_w, &cell_h);

  if (tcgetattr(STDIN_FILENO, &g_saved) != 0) {
    fprintf(stderr, "sl0ppty: not a terminal\n");
    return 1;
  }
  g_saved_ok = true;
  struct termios t = g_saved;
  cfmakeraw(&t);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
  atexit(restore);
  write_all(STDOUT_FILENO, TERM_ENTER, strlen(TERM_ENTER));

  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGWINCH, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);

  send_size(fd, MSG_HELLO, cols, rows, cell_w, cell_h);

  msg_reader_t reader;
  msg_reader_init(&reader);
  int exit_reason = -1;

  while (!g_quit) {
    struct pollfd pfds[2] = {
        {.fd = STDIN_FILENO, .events = POLLIN},
        {.fd = fd, .events = POLLIN},
    };
    int n = poll(pfds, 2, -1);

    if (g_winch) {
      g_winch = 0;
      /* A resize can also be a move to another monitor, so the cell size is
       * re-read rather than remembered from the first frame. */
      term_size(&cols, &rows, &cell_w, &cell_h);
      send_size(fd, MSG_RESIZE, cols, rows, cell_w, cell_h);
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[0].revents & POLLIN) {
      uint8_t buf[65536];
      ssize_t got = read(STDIN_FILENO, buf, sizeof buf);
      if (got > 0 && msg_send(fd, MSG_INPUT, buf, (size_t)got) != 0) break;
      if (got == 0) break;
    }

    if (pfds[1].revents & (POLLIN | POLLHUP)) {
      uint8_t buf[65536];
      ssize_t got = read(fd, buf, sizeof buf);
      if (got <= 0) break;
      msg_reader_feed(&reader, buf, (size_t)got);
      msg_t m;
      while (msg_reader_next(&reader, &m)) {
        if (m.type == MSG_OUTPUT) {
          write_all(STDOUT_FILENO, m.data, m.len);
        } else if (m.type == MSG_EXIT) {
          exit_reason = m.len ? m.data[0] : EXIT_SESSION_ENDED;
          g_quit = 1;
          break;
        }
      }
    }
  }

  msg_reader_free(&reader);
  close(fd);
  restore();
  g_saved_ok = false;

  if (exit_reason == EXIT_DETACHED)
    fprintf(stderr, "[detached]\n");
  else if (exit_reason == EXIT_REPLACED)
    fprintf(stderr, "[replaced by another client]\n");
  return 0;
}

/* One-shot control request: connect, ask, print the answer. */
int client_control(int fd, const char *json) {
  if (msg_send(fd, MSG_CMD, json, strlen(json)) != 0) return 1;
  msg_reader_t reader;
  msg_reader_init(&reader);
  uint8_t buf[65536];
  int rc = 1;
  for (;;) {
    ssize_t got = read(fd, buf, sizeof buf);
    if (got <= 0) break;
    msg_reader_feed(&reader, buf, (size_t)got);
    msg_t m;
    bool done = false;
    while (msg_reader_next(&reader, &m)) {
      if (m.type == MSG_REPLY) {
        fwrite(m.data, 1, m.len, stdout);
        fputc('\n', stdout);
        rc = 0;
        done = true;
        break;
      }
    }
    if (done) break;
  }
  msg_reader_free(&reader);
  close(fd);
  return rc;
}
