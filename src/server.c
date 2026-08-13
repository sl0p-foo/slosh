/* The server: owns the panes and the layout, renders, and talks to at most one
 * attached client (D10). It outlives every client, which is the entire point —
 * agents keep running while you are gone. */
#define _GNU_SOURCE
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "cmd.h"
#include "proto.h"

#include <sys/wait.h>

#define MAX_PANES 64
#define FRAME_MS 8
#define ESC_MS 40

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { g_stop = 1; }

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Connections are not clients. A connection becomes *the* display client only
 * by sending MSG_HELLO; anything else (a control command, a liveness probe
 * from `ls`) is just a connection and never displaces anybody.
 *
 * This distinction is not fussiness. The zellij fork sl0ptty replaces had a
 * 20-25%% CLI failure rate because every action probed liveness by connecting,
 * that probe counted as a client, and its teardown removed the *real* one. */
#define MAX_CONNS 16

typedef struct {
  int fd;
  msg_reader_t reader;
  bool display;
} conn_t;

typedef struct {
  app_t *app;
  screen_t screen;
  input_parser_t *in;
  conn_t conns[MAX_CONNS];
  size_t nconns;
} server_t;

static conn_t *display_conn(server_t *s) {
  for (size_t i = 0; i < s->nconns; i++)
    if (s->conns[i].display) return &s->conns[i];
  return NULL;
}

static void conn_close(server_t *s, size_t i) {
  close(s->conns[i].fd);
  msg_reader_free(&s->conns[i].reader);
  s->conns[i] = s->conns[--s->nconns];
}

static void conn_drop(server_t *s, size_t i, uint8_t reason) {
  msg_send(s->conns[i].fd, MSG_EXIT, &reason, 1);
  conn_close(s, i);
}

static void srv_event(const input_event_t *ev, void *ud) {
  app_event(((server_t *)ud)->app, ev);
}

static int listen_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  unlink(path);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  chmod(path, 0600);
  if (listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Send whatever the last compose produced to the display client. */
static void push_frame(server_t *s) {
  conn_t *c = display_conn(s);
  if (!c) return;
  screen_render(&s->screen);
  if (!s->screen.out_len) return;
  if (msg_send(c->fd, MSG_OUTPUT, s->screen.out, s->screen.out_len) != 0)
    for (size_t i = 0; i < s->nconns; i++)
      if (&s->conns[i] == c) {
        conn_close(s, i);
        break;
      }
}

static void drop_display(server_t *s, uint8_t reason) {
  for (size_t i = 0; i < s->nconns; i++)
    if (s->conns[i].display) {
      conn_drop(s, i, reason);
      return;
    }
}

int server_run(const char *name, const char *const argv[], uint16_t cols,
               uint16_t rows, const char *layout) {
  char path[512];
  if (session_socket_path(name, path, sizeof path) != 0) return 1;
  int lfd = listen_socket(path);
  if (lfd < 0) {
    fprintf(stderr, "sl0ptty: cannot listen on %s: %s\n", path, strerror(errno));
    return 1;
  }

  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

  server_t s = {0};
  s.app = app_new(argv, cols, rows);
  if (!s.app) return 1;
  if (layout) {
    char err[256] = {0};
    if (!app_apply_layout_file(s.app, layout, true, err, sizeof err))
      fprintf(stderr, "sl0ptty: %s: %s\n", layout, err[0] ? err : "bad layout");
  }
  app_resize(s.app, cols, rows);
  screen_init(&s.screen, cols, rows);
  s.in = input_new();

  bool pending_paint = true;
  int64_t next_frame = now_ms();
  int64_t esc_due = 0;

  while (!g_stop && !app_should_quit(s.app)) {
    int fds[MAX_PANES];
    size_t npanes = app_fds(s.app, fds, MAX_PANES);
    struct pollfd pfds[MAX_PANES + MAX_CONNS + 1];
    size_t n = 0;
    pfds[n++] = (struct pollfd){.fd = lfd, .events = POLLIN};
    size_t conn_slot = n;
    for (size_t i = 0; i < s.nconns; i++)
      pfds[n++] = (struct pollfd){.fd = s.conns[i].fd, .events = POLLIN};
    size_t pane_slot = n;
    for (size_t i = 0; i < npanes; i++)
      pfds[n++] = (struct pollfd){.fd = fds[i], .events = POLLIN};

    int timeout = -1;
    if (pending_paint) {
      int64_t due = next_frame - now_ms();
      timeout = due <= 0 ? 0 : (int)due;
    }
    if (input_pending(s.in)) {
      int64_t due = esc_due - now_ms();
      int t = due <= 0 ? 0 : (int)due;
      if (timeout < 0 || t < timeout) timeout = t;
    }

    int r = poll(pfds, (nfds_t)n, timeout);
    if (r < 0 && errno != EINTR) break;

    if (pfds[0].revents & POLLIN) {
      int c = accept(lfd, NULL, NULL);
      if (c >= 0) {
        if (s.nconns == MAX_CONNS) {
          close(c);
        } else {
          conn_t *nc = &s.conns[s.nconns++];
          nc->fd = c;
          nc->display = false;
          msg_reader_init(&nc->reader);
        }
      }
    }

    for (size_t ci = 0; ci < s.nconns;) {
      struct pollfd *pf = &pfds[conn_slot + ci];
      if (pf->fd != s.conns[ci].fd || !(pf->revents & (POLLIN | POLLHUP))) {
        ci++;
        continue;
      }
      uint8_t buf[65536];
      ssize_t got = read(s.conns[ci].fd, buf, sizeof buf);
      if (got <= 0) {
        conn_close(&s, ci); /* gone: the session keeps running regardless */
        continue;
      }
      msg_reader_feed(&s.conns[ci].reader, buf, (size_t)got);
      msg_t m;
      bool closed = false;
      while (!closed && msg_reader_next(&s.conns[ci].reader, &m)) {
        switch (m.type) {
          case MSG_HELLO:
          case MSG_RESIZE:
            if (m.type == MSG_HELLO && !s.conns[ci].display) {
              /* now it is a client: the previous display is displaced */
              for (size_t j = 0; j < s.nconns; j++)
                if (j != ci && s.conns[j].display) {
                  conn_drop(&s, j, EXIT_REPLACED);
                  if (j < ci) ci--; /* the array compacted under us */
                  break;
                }
              s.conns[ci].display = true;
            }
            if (m.len >= 4 && s.conns[ci].display) {
              uint16_t c = (uint16_t)(m.data[0] << 8 | m.data[1]);
              uint16_t rr = (uint16_t)(m.data[2] << 8 | m.data[3]);
              if (c && rr) {
                screen_resize(&s.screen, c, rr);
                app_resize(s.app, c, rr);
              }
            }
            s.screen.force_full = true; /* a fresh client knows nothing */
            pending_paint = true;
            next_frame = now_ms();
            break;
          case MSG_INPUT:
            if (!s.conns[ci].display) break; /* only the client types */
            input_feed(s.in, m.data, m.len, srv_event, &s);
            esc_due = now_ms() + ESC_MS;
            pending_paint = true;
            break;
          case MSG_DETACH:
            conn_drop(&s, ci, EXIT_DETACHED);
            closed = true;
            break;
          case MSG_CMD: {
            /* The same vocabulary the headless driver speaks (cmd.c), so a
             * script written against one works against the other. */
            bool q = false;
            char *reply =
                cmd_exec(s.app, &s.screen, s.in, (const char *)m.data, &q);
            const char *body = reply ? reply : "{\"error\":\"unknown command\"}";
            msg_send(s.conns[ci].fd, MSG_REPLY, body, strlen(body));
            free(reply);
            if (q) g_stop = 1;
            s.screen.force_full = true; /* it may have changed the layout */
            pending_paint = true;
            next_frame = now_ms();
            break;
          }
          default:
            break;
        }
      }
      if (!closed) ci++;
    }

    if (input_pending(s.in) && now_ms() >= esc_due)
      input_timeout(s.in, srv_event, &s);

    for (size_t i = 0; i < npanes; i++) {
      if (pfds[pane_slot + i].revents & (POLLIN | POLLHUP)) {
        app_pump_fd(s.app, pfds[pane_slot + i].fd);
        if (!pending_paint) {
          pending_paint = true;
          next_frame = now_ms() + FRAME_MS;
        }
      }
    }
    app_reap(s.app);

    if (app_detach_requested(s.app)) {
      app_clear_detach(s.app);
      drop_display(&s, EXIT_DETACHED);
    }

    if (pending_paint && now_ms() >= next_frame) {
      app_compose(s.app, &s.screen);
      push_frame(&s);
      pending_paint = false;
      next_frame = now_ms();
    }
  }

  while (s.nconns) conn_drop(&s, 0, EXIT_SESSION_ENDED);
  unlink(path);
  close(lfd);
  input_free(s.in);
  screen_free(&s.screen);
  app_free(s.app);
  return 0;
}

/* Fork a server into the background and wait for its socket to answer. */
int server_spawn(const char *name, const char *const argv[], uint16_t cols,
                 uint16_t rows, const char *layout) {
  char path[512], logp[512];
  if (session_socket_path(name, path, sizeof path) != 0) return -1;
  session_log_path(name, logp, sizeof logp);

  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    setsid();
    if (fork() != 0) _exit(0); /* orphan the server so no one waits on it */
    int null = open("/dev/null", O_RDWR);
    int log = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(null, STDIN_FILENO);
    dup2(log >= 0 ? log : null, STDOUT_FILENO);
    dup2(log >= 0 ? log : null, STDERR_FILENO);
    if (null > 2) close(null);
    if (log > 2) close(log);
    _exit(server_run(name, argv, cols, rows, layout));
  }
  waitpid(pid, NULL, 0); /* the intermediate exits immediately */

  /* Wait for the socket to accept, rather than sleeping and hoping. */
  for (int i = 0; i < 400; i++) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) return fd;
    close(fd);
    struct timespec ts = {.tv_nsec = 5 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  return -1;
}

int server_connect(const char *name) {
  char path[512];
  if (session_socket_path(name, path, sizeof path) != 0) return -1;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
