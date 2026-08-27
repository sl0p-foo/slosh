/* The server: owns the panes and the layout, renders, and talks to at most one
 * attached client (D10). It outlives every client, which is the entire point —
 * agents keep running while you are gone. */
#define _GNU_SOURCE
#include "server.h"

#ifdef __APPLE__
#include <mach-o/dyld.h> /* _NSGetExecutablePath: Darwin has no /proc/self/exe */
#endif

#include "config.h"

/* The config-file watcher speaks the OS's native change-notification API:
 * inotify on Linux, kqueue's EVFILT_VNODE on macOS. Both feed one fd that
 * server_run() adds to its poll set (a kqueue descriptor is pollable), so the
 * only platform-specific code is registering directories and draining events. */
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/inotify.h>
#endif

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
 * This distinction is not fussiness. The zellij fork slosh replaces had a
 * 20-25%% CLI failure rate because every action probed liveness by connecting,
 * that probe counted as a client, and its teardown removed the *real* one. */
#define MAX_CONNS 16

/* A connection that has stopped reading must not stop the session.
 *
 * Everything the server says used to go out through a blocking sendmsg() that
 * looped until the kernel took all of it. A display whose reader had paused --
 * C-s at the terminal, a stopped ssh, a client swapping -- filled its socket
 * buffer and then the *server* blocked inside that loop: every other pane, the
 * config watcher and the control socket all stopped with it, and a session that
 * is supposed to outlive its client hung on one. On Linux 64K of socket buffer
 * hid it for long enough to look like it worked; macOS buffers are a fraction
 * of that and it wedges within a screenful.
 *
 * So output is queued and flushed when the socket says it can take more. The
 * cap is what makes it bounded: past it we are not buffering a slow reader any
 * more, we are storing a screen nobody is looking at, and the connection is
 * dropped. A display that comes back reattaches and gets a full repaint, which
 * is the same thing that happens after any detach. */
#define MAX_OUTBOX (4u * 1024 * 1024)

typedef struct {
  int fd;
  msg_reader_t reader;
  bool display;
  uint8_t *out; /* queued bytes, already framed */
  size_t out_len, out_off, out_cap;
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
  free(s->conns[i].out);
  s->conns[i] = s->conns[--s->nconns];
}

/* Push what is queued. Returns false when the connection is finished with --
 * the peer is gone, or it is so far behind that keeping up is not the word for
 * what we would be doing. EAGAIN is not a failure: it is the socket saying to
 * come back when poll() says so. */
static bool conn_flush(conn_t *c) {
  while (c->out_off < c->out_len) {
    ssize_t w =
        send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    c->out_off += (size_t)w;
  }
  c->out_len = c->out_off = 0;
  return true;
}

/* Frame a message onto the queue, then try to get rid of it immediately: the
 * common case is a socket with room, and queueing is only the fallback. */
static bool conn_send(conn_t *c, uint8_t type, const void *data, size_t len) {
  size_t need = c->out_len + MSG_HDR + len;
  if (need - c->out_off > MAX_OUTBOX) return false;
  if (c->out_off && need > c->out_cap) { /* reclaim what has already gone */
    memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
    c->out_len -= c->out_off;
    c->out_off = 0;
    need = c->out_len + MSG_HDR + len;
  }
  if (need > c->out_cap) {
    size_t cap = c->out_cap ? c->out_cap : 8192;
    while (cap < need) cap *= 2;
    uint8_t *grown = realloc(c->out, cap);
    if (!grown) return false;
    c->out = grown;
    c->out_cap = cap;
  }
  uint8_t hdr[MSG_HDR] = {type, (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                          (uint8_t)(len >> 8), (uint8_t)len};
  memcpy(c->out + c->out_len, hdr, MSG_HDR);
  c->out_len += MSG_HDR;
  if (len) {
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
  }
  return conn_flush(c);
}

static void conn_drop(server_t *s, size_t i, uint8_t reason) {
  conn_send(&s->conns[i], MSG_EXIT, &reason, 1);
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

/* base64, for OSC 52. */
static char *b64(const char *in, size_t len, size_t *out_len) {
  static const char T[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t n = ((len + 2) / 3) * 4;
  char *out = malloc(n + 1);
  size_t o = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned v = (unsigned char)in[i] << 16;
    if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
    if (i + 2 < len) v |= (unsigned char)in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = i + 1 < len ? T[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < len ? T[v & 63] : '=';
  }
  out[o] = 0;
  *out_len = o;
  return out;
}

/* Images go out after the cell diff, so a repainted cell cannot land on top
 * of a placement we just made. */
static void push_graphics(server_t *s) {
  conn_t *c = display_conn(s);
  if (!c) return;
  size_t len = 0;
  const char *bytes = app_graphics(s->app, &len);
  if (len) conn_send(c, MSG_OUTPUT, bytes, len);
}

/* The clipboard lives on the client's machine, not ours, so a copy travels as
 * OSC 52 for the outer terminal to honour. */
static void push_clipboard(server_t *s) {
  char *text = app_take_clipboard(s->app);
  if (!text) return;
  conn_t *c = display_conn(s);
  if (c) {
    size_t b64len = 0;
    char *enc = b64(text, strlen(text), &b64len);
    size_t n = b64len + 32;
    char *msg = malloc(n);
    int len = snprintf(msg, n, "\x1b]52;c;%s\x1b\\", enc);
    if (len > 0) conn_send(c, MSG_OUTPUT, msg, (size_t)len);
    free(msg);
    free(enc);
  }
  free(text);
}

/* Send whatever the last compose produced to the display client. */
static void push_frame(server_t *s) {
  conn_t *c = display_conn(s);
  if (!c) return;
  screen_render(&s->screen);
  if (!s->screen.out_len) return;
  if (!conn_send(c, MSG_OUTPUT, s->screen.out, s->screen.out_len))
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

/* What the config watcher is watching: a directory per config file, and the
 * basenames to match events against. Rebuilt from the config every time one
 * loads, because an `include` line can be added, changed or removed — a theme
 * you can include is a theme that has to reload when you save it.
 *
 * Names are matched against every watched directory rather than against the one
 * their own watch fired for. Two directories holding a file of the same name
 * would reload for the other's save, which costs a debounced re-read of a config
 * that has not changed and is not worth a second data structure to avoid. */
#define WATCH_MAX CONFIG_FILES_MAX
typedef struct {
  int fd;
  /* Two slots per file: on macOS each config file is watched twice, once via
   * its directory and once via the file itself (see watch_config). */
  int wds[WATCH_MAX * 2];
  size_t nwds;
  char names[WATCH_MAX][128];
  size_t nnames;
} watchset_t;

static void watch_config(watchset_t *w) {
  if (w->fd < 0) return;
#ifdef __APPLE__
  /* kqueue drops a vnode filter when its fd closes, so closing is the unwatch. */
  for (size_t i = 0; i < w->nwds; i++) close(w->wds[i]);
#else
  for (size_t i = 0; i < w->nwds; i++) inotify_rm_watch(w->fd, w->wds[i]);
#endif
  w->nwds = 0;
  w->nnames = 0;

  const char *files[WATCH_MAX];
  size_t n = app_config_files(files, WATCH_MAX);
  for (size_t i = 0; i < n; i++) {
    char dir[512], base[128];
    snprintf(dir, sizeof dir, "%s", files[i]);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = 0;
      snprintf(base, sizeof base, "%s", slash + 1);
    } else {
      snprintf(base, sizeof base, "%s", files[i]);
      snprintf(dir, sizeof dir, ".");
    }
    /* The main config's directory is ours to create, so that a session started
     * before the file exists still notices it appearing. An include's is not:
     * a mistyped path should not leave a directory behind. */
    if (i == 0) path_mkdirs(dir);

#ifdef __APPLE__
    /* kqueue's EVFILT_VNODE reports events per *vnode*, so unlike inotify a
     * directory watch sees entries created, renamed or deleted but NOT a write
     * into a file that already exists. So we watch two things: the directory
     * (which catches the temp-file-then-rename most editors do, and a file
     * appearing for the first time) and the file itself when it exists (which
     * catches an in-place rewrite). A rename swaps the inode out from under the
     * file watch, but the directory watch fires, we reload, and watch_config
     * re-opens the new inode -- so the pair survives both save styles.
     * O_EVTONLY opens for notification only and never pins a volume. It reports
     * no filename, so any event triggers the debounced re-read, which decides
     * for itself whether anything we care about changed. */
    const unsigned vnflags =
        NOTE_WRITE | NOTE_EXTEND | NOTE_RENAME | NOTE_DELETE | NOTE_ATTRIB;
    const char *targets[2] = {dir, files[i]};
    for (size_t t = 0; t < 2; t++) {
      int vfd = open(targets[t], O_EVTONLY);
      if (vfd < 0) continue;
      struct kevent ev;
      EV_SET(&ev, vfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, vnflags, 0, NULL);
      if (kevent(w->fd, &ev, 1, NULL, 0, NULL) == 0 && w->nwds < WATCH_MAX * 2)
        w->wds[w->nwds++] = vfd;
      else
        close(vfd);
    }
#else
    int wd =
        inotify_add_watch(w->fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (wd >= 0) {
      bool have = false;
      for (size_t k = 0; k < w->nwds; k++)
        if (w->wds[k] == wd) have = true; /* same directory, same descriptor */
      if (!have && w->nwds < WATCH_MAX) w->wds[w->nwds++] = wd;
    }
#endif
    if (w->nnames < WATCH_MAX) {
      snprintf(w->names[w->nnames], sizeof w->names[0], "%s", base);
      w->nnames++;
    }
  }
}

#ifndef __APPLE__
static bool watch_hit(const watchset_t *w, const char *name) {
  for (size_t i = 0; i < w->nnames; i++)
    if (strcmp(w->names[i], name) == 0) return true;
  return false;
}
#endif

/* Drain everything the watch fd has queued and report whether any of it touched
 * a config file. Called when poll() marks the watch fd readable; the caller
 * debounces, because one save is not always one event. */
static bool watch_drain(watchset_t *w) {
  bool touched = false;
#ifdef __APPLE__
  struct kevent evs[16];
  struct timespec zero = {0, 0};
  for (;;) {
    int n = kevent(w->fd, NULL, 0, evs, 16, &zero);
    if (n <= 0) break;
    touched = true; /* any vnode event means a watched directory changed */
    if (n < 16) break;
  }
#else
  char buf[4096];
  for (;;) {
    ssize_t got = read(w->fd, buf, sizeof buf);
    if (got <= 0) break;
    for (char *q = buf; q < buf + got;) {
      struct inotify_event *ev = (struct inotify_event *)q;
      if (ev->len && watch_hit(w, ev->name)) touched = true;
      q += sizeof *ev + ev->len;
    }
  }
#endif
  return touched;
}

/* What every pane is told about the session around it. Panes inherit this
 * process's environment, so this is how a program *inside* one learns which
 * session it is in -- and therefore which socket to talk to. `SLOSH=1`
 * (pty.c) says "you are in a pane"; this says which one of possibly several,
 * which is the part a program cannot work out for itself: `ls` lists them all
 * and none of them is labelled "the one you are in".
 *
 * NULL for a mode with no socket, and that case is why this is a function both
 * modes call rather than two setenvs in the mode that has one. `--script` has
 * always been *documented* as leaving `SLOSH_SESSION` unset -- but a value it
 * never set was a value it inherited, and a script driver started from inside a
 * pane handed its own panes the *outer* session's name. Anything following the
 * documented recipe (`-s ${SLOSH_SESSION:-main} cmd`) then sent its commands
 * to a session it was not in, confidently, because the name it read was real.
 *
 * `SLOSH_BIN` is set in both modes for the same reason it is set in either: a
 * program in a pane that finds `slosh` missing from its PATH -- a session
 * started from a build tree, an agent with a trimmed environment -- can
 * otherwise see the session it is in and have no way to reach it. Read from
 * /proc rather than argv[0], which is whatever the caller felt like passing,
 * and set rather than inherited so it names the binary that actually made this
 * pane, not one that made some pane somewhere. */
void session_env(const char *name) {
  if (name)
    setenv("SLOSH_SESSION", name, 1);
  else
    unsetenv("SLOSH_SESSION");
  char self[512];
#ifdef __APPLE__
  /* Darwin's answer to /proc/self/exe. What it hands back can still contain
   * symlinks and .., so realpath() finishes the job. */
  char raw[512];
  uint32_t sz = sizeof raw;
  if (_NSGetExecutablePath(raw, &sz) == 0 && realpath(raw, self))
    setenv("SLOSH_BIN", self, 1);
#else
  ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
  if (sn > 0) {
    self[sn] = 0;
    setenv("SLOSH_BIN", self, 1);
  }
#endif
}

int server_run(const char *name, const char *const argv[], uint16_t cols,
               uint16_t rows, const char *layout, bool watch) {
  char path[512];
  if (session_socket_path(name, path, sizeof path) != 0) return 1;
  int lfd = listen_socket(path);
  if (lfd < 0) {
    fprintf(stderr, "slosh: cannot listen on %s: %s\n", path, strerror(errno));
    return 1;
  }

  session_env(name);

  struct sigaction sa = {.sa_handler = on_sig};
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  /* SIG_DFL, not SIG_IGN. Both leave us free of a handler, but SIG_IGN also
   * tells the kernel to discard exit statuses: children are reaped where we
   * cannot see them, and every pane then dies of "exited" with no idea what
   * it exited *with*. The headless driver never set it, so this was invisible
   * to the whole test suite and showed up the first time a real session was
   * looked at. SIGCHLD's default action is to be discarded, so nothing is
   * interrupted and the only difference is that a status waits to be read. */
  signal(SIGCHLD, SIG_DFL);

  server_t s = {0};
  s.app = app_new(argv, cols, rows);
  if (s.app) app_set_session(s.app, name);
  if (!s.app) return 1;
  if (layout) {
    char err[256] = {0};
    if (!app_apply_layout_file(s.app, layout, true, err, sizeof err))
      fprintf(stderr, "slosh: %s: %s\n", layout, err[0] ? err : "bad layout");
  }
  app_resize(s.app, cols, rows);
  screen_init(&s.screen, cols, rows);
  s.in = input_new();

  app_compose(s.app, &s.screen); /* a click resolves against a painted frame */

  /* A config that loaded with a complaint in it still started the session, so
   * the complaint has nowhere to go but here. Once, at startup: the log line is
   * written where nobody attached will see it. */
  {
    const char *why = app_config_complaint();
    if (why && *why) app_toast(s.app, why);
  }

  /* Watch the config's *directory*, not the file. Editors overwhelmingly save
   * by writing a temporary file and renaming it over the target, which swaps
   * the inode out from under a watch on the file itself: it fires once and
   * then never again, which is worse than not watching at all because it looks
   * like it works. Watching the directory and filtering by name survives that,
   * and picks up a file that did not exist when the session started. */
  /* One save is not always one event: an editor that creates the file rather
   * than rewriting it produces CREATE and then CLOSE_WRITE, and reloading on
   * each would parse the config twice and announce it twice for one edit.
   * Waiting a moment for the file to stop changing also means an editor that
   * writes in chunks is read once it has finished rather than halfway. */
  const int64_t RELOAD_DEBOUNCE_MS = 80;
  int64_t reload_due = -1;
  int inofd = -1;
  watchset_t watches = {.fd = -1};
  if (watch) {
#ifdef __APPLE__
    inofd = kqueue();
    if (inofd >= 0) fcntl(inofd, F_SETFD, FD_CLOEXEC);
#else
    inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    watches.fd = inofd;
    watch_config(&watches);
  }

  bool pending_paint = true;
  int64_t next_frame = now_ms();
  int64_t esc_due = 0;

  while (!g_stop && !app_should_quit(s.app)) {
    int fds[MAX_PANES];
    size_t npanes = app_fds(s.app, fds, MAX_PANES);
    struct pollfd pfds[MAX_PANES + MAX_CONNS + 2]; /* +listen +inotify */
    size_t n = 0;
    pfds[n++] = (struct pollfd){.fd = lfd, .events = POLLIN};
    size_t conn_slot = n;
    for (size_t i = 0; i < s.nconns; i++)
      pfds[n++] = (struct pollfd){
          .fd = s.conns[i].fd,
          .events =
              (short)(POLLIN |
                      (s.conns[i].out_off < s.conns[i].out_len ? POLLOUT : 0))};
    size_t pane_slot = n;
    for (size_t i = 0; i < npanes; i++)
      pfds[n++] = (struct pollfd){.fd = fds[i], .events = POLLIN};
    size_t ino_slot = n;
    if (inofd >= 0) pfds[n++] = (struct pollfd){.fd = inofd, .events = POLLIN};

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
    /* some things happen without an event: a toast expiring, a hover guide
     * arming under a pointer that is deliberately not moving */
    if (reload_due >= 0) {
      int64_t due = reload_due - now_ms();
      int t = due <= 0 ? 0 : (int)due;
      if (timeout < 0 || t < timeout) timeout = t;
    }
    int self_due = app_next_deadline_ms(s.app);
    if (self_due >= 0 && (timeout < 0 || self_due < timeout)) {
      timeout = self_due;
      pending_paint = true;
    }

    int r = poll(pfds, (nfds_t)n, timeout);
    if (r < 0 && errno != EINTR) break;

    if (pfds[0].revents & POLLIN) {
      int c = accept(lfd, NULL, NULL);
      if (c >= 0) {
        if (s.nconns == MAX_CONNS) {
          close(c);
        } else {
          /* Non-blocking: with a queue behind it, a write that cannot
           * complete now is a thing to finish later rather than to wait for. */
          int fl = fcntl(c, F_GETFL, 0);
          if (fl >= 0) fcntl(c, F_SETFL, fl | O_NONBLOCK);
          conn_t *nc = &s.conns[s.nconns++];
          *nc = (conn_t){.fd = c};
          msg_reader_init(&nc->reader);
        }
      }
    }

    for (size_t ci = 0; ci < s.nconns;) {
      struct pollfd *pf = &pfds[conn_slot + ci];
      if (pf->fd != s.conns[ci].fd) {
        ci++;
        continue;
      }
      if (pf->revents & POLLOUT) {
        if (!conn_flush(&s.conns[ci])) {
          conn_close(&s,
                     ci); /* gone, or too far behind to still be a display */
          continue;
        }
      }
      if (!(pf->revents & (POLLIN | POLLHUP))) {
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
            /* Someone is now looking, which is the one moment a greeting
             * makes sense: a headless or scripted session never gets here. */
            app_splash(s.app);
          }
          if (m.len >= 4 && s.conns[ci].display) {
            uint16_t c = (uint16_t)(m.data[0] << 8 | m.data[1]);
            uint16_t rr = (uint16_t)(m.data[2] << 8 | m.data[3]);
            if (c && rr) {
              screen_resize(&s.screen, c, rr);
              app_resize(s.app, c, rr);
            }
            /* The cell size arrived with it, from a client new enough to
               * send one and a terminal willing to say. Zero means neither,
               * and app_set_cell_px leaves the default standing. */
            if (m.len >= 8) {
              uint16_t cw = (uint16_t)(m.data[4] << 8 | m.data[5]);
              uint16_t ch = (uint16_t)(m.data[6] << 8 | m.data[7]);
              app_set_cell_px(s.app, cw, ch);
            }
          }
          s.screen.force_full = true; /* a fresh client knows nothing */
          app_graphics_reset(s.app);  /* including any image we had sent */
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
          conn_send(&s.conns[ci], MSG_REPLY, body, strlen(body));
          free(reply);
          if (q) g_stop = 1;
          s.screen.force_full = true; /* it may have changed the layout */
          pending_paint = true;
          next_frame = now_ms();
          break;
        }
        default: break;
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
    /* The config changed on disk. Reloading is the same call the `reload`
     * command makes, including its fail-open behaviour: a file that does not
     * parse leaves the running config alone and says so, which matters more
     * here than there because an editor saving halfway through a change is a
     * normal thing to see rather than an operator mistake. */
    if (inofd >= 0 && (pfds[ino_slot].revents & POLLIN)) {
      if (watch_drain(&watches)) reload_due = now_ms() + RELOAD_DEBOUNCE_MS;
    }

    if (reload_due >= 0 && now_ms() >= reload_due) {
      reload_due = -1;
      char err[256] = {0};
      if (app_reload_config(err, sizeof err)) {
        app_resize(s.app, s.screen.cols, s.screen.rows);
        /* The set of files can have changed with the config that named them:
         * an include added, pointed somewhere else, or taken out. */
        watch_config(&watches);
        /* A complaint rather than "config reloaded": the file applied, and one
         * line of it did not. Saying only the good half is how a mistyped
         * include becomes ten minutes of wondering. */
        const char *why = app_config_complaint();
        app_toast(s.app, why && *why ? why : "config reloaded");
      } else {
        app_toast(s.app, err[0] ? err : "config reload failed");
      }
      s.screen.force_full = true;
      pending_paint = true;
      next_frame = now_ms();
    }

    app_reap(s.app);

    /* The event-less work: a selection drag held past a pane's edge scrolls
     * on a clock (app_next_deadline_ms is what woke the poll for it), and a
     * step that moved anything is a frame the client is owed. */
    if (app_tick(s.app) && !pending_paint) {
      pending_paint = true;
      next_frame = now_ms();
    }

    if (app_detach_requested(s.app)) {
      app_clear_detach(s.app);
      drop_display(&s, EXIT_DETACHED);
    }

    if (pending_paint && now_ms() >= next_frame) {
      app_compose(s.app, &s.screen);
      push_frame(&s);
      push_graphics(&s);
      push_clipboard(&s);
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
                 uint16_t rows, const char *layout, bool watch) {
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
    _exit(server_run(name, argv, cols, rows, layout, watch));
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
