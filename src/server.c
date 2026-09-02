/* The server: owns the panes and shared layout, renders once, and projects that
 * screen into every attached client. It outlives all of them, which is the
 * entire point — agents keep running while you are gone. */
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
 * more, and new frames are *dropped* rather than queued -- the model remembers
 * what it owes (force_full for cells, gfx_commit for images) and says it again
 * once the backlog drains. Dropping used to mean closing the connection, but a
 * backlog over the cap is not a dead client: one kitty image retransmission is
 * megabytes in a single indivisible message, so the queue sits over the cap
 * for exactly as long as a healthy client needs to swallow it -- and closing
 * there detached the session every time a screenshot crossed the wire. */
#define MAX_OUTBOX (4u * 1024 * 1024)

typedef struct {
  int fd;
  msg_reader_t reader;
  bool display;
  uint16_t cols, rows, cell_w, cell_h;
  uint16_t view_x, view_y, shown_cols, shown_rows;
  uint64_t activity;
  input_parser_t *in;
  int64_t esc_due;
  screen_t view;
  bool view_init;
  graphics_t *gfx;
  uint8_t *out; /* queued bytes, already framed */
  size_t out_len, out_off, out_cap;
} conn_t;

typedef struct {
  app_t *app;
  screen_t screen;
  input_parser_t *cmd_in;
  conn_t conns[MAX_CONNS];
  size_t nconns;
  int active_fd;
  uint64_t activity_seq;
  uint16_t fallback_cell_w, fallback_cell_h;
  bool pending_paint;
  int64_t next_frame;
} server_t;

static conn_t *active_conn(server_t *s) {
  for (size_t i = 0; i < s->nconns; i++)
    if (s->conns[i].display && s->conns[i].fd == s->active_fd)
      return &s->conns[i];
  return NULL;
}

static size_t display_count(const server_t *s) {
  size_t n = 0;
  for (size_t i = 0; i < s->nconns; i++)
    if (s->conns[i].display) n++;
  return n;
}

static bool conn_close(server_t *s, size_t i) {
  bool active = s->conns[i].fd == s->active_fd;
  close(s->conns[i].fd);
  msg_reader_free(&s->conns[i].reader);
  input_free(s->conns[i].in);
  if (s->conns[i].view_init) screen_free(&s->conns[i].view);
  gfx_free(s->conns[i].gfx);
  free(s->conns[i].out);
  s->conns[i] = s->conns[--s->nconns];
  if (active) s->active_fd = -1;
  return active;
}

/* Push what is queued. Returns false when the connection is finished with --
 * the peer is gone, or it is so far behind that keeping up is not the word for
 * what we would be doing. EAGAIN is not a failure: it is the socket saying to
 * come back when poll() says so. */
#ifdef _WIN32
/* There is no SIGPIPE to suppress, so there is nothing for MSG_NOSIGNAL to
 * ask for: a send to a closed socket already just returns an error. */
#define MSG_NOSIGNAL 0
#endif
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
  /* The cap bounds the *backlog*, not the message: a queue already MAX_OUTBOX
   * deep is a client that stopped reading, but a single big message on an
   * empty queue is just a big message. A kitty image retransmission is
   * megabytes of base64 in one indivisible stream -- capping the message
   * itself made every screenshot larger than the cap silently undeliverable,
   * forever. Memory stays bounded at MAX_OUTBOX plus one message. */
  if (c->out_len - c->out_off > MAX_OUTBOX) return false;
  size_t need = c->out_len + MSG_HDR + len;
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

static bool conn_drop(server_t *s, size_t i, uint8_t reason) {
  conn_send(&s->conns[i], MSG_EXIT, &reason, 1);
  return conn_close(s, i);
}

static void request_paint(server_t *s, bool immediate) {
  s->pending_paint = true;
  if (immediate || !s->next_frame) s->next_frame = now_ms();
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
 * of a placement we just made.
 *
 * Returns whether the client is caught up. The stream is indivisible -- half
 * a frame is a dangling APC that eats whatever follows it -- so a full outbox
 * drops the whole frame, the model is told (deletions stay owed, transmits
 * are re-sent), and the caller keeps a repaint pending until it lands. Losing
 * a frame here silently was how a scrolled-away screenshot stayed parked on
 * the screen: its deletion left with the dropped bytes and was never owed
 * again. */
static bool push_graphics(server_t *s, conn_t *c) {
  size_t len = 0;
  const char *bytes = app_graphics_view(s->app, c->gfx, c->view_x, c->view_y,
                                        c->cols, c->rows, &len);
  if (!len) {
    app_graphics_view_commit(c->gfx, true);
    return true;
  }
  bool ok = conn_send(c, MSG_OUTPUT, bytes, len);
  app_graphics_view_commit(c->gfx, ok);
  return ok;
}

/* The clipboard lives on the client's machine, not ours, so a copy travels as
 * OSC 52 for the outer terminal to honour. Input-driven copies name their
 * originating connection; asynchronous pane copies use the active one. */
static void push_clipboard_to(server_t *s, conn_t *c) {
  char *text = app_take_clipboard(s->app);
  if (!text) return;
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

static void push_clipboard(server_t *s) {
  push_clipboard_to(s, active_conn(s));
}

/* Send whatever the last compose produced to one display client. */
/* Returns whether this client has the frame.
 * A refused send is a full outbox, not a dead client: the diff that was not
 * queued is lost, so the client's cells are unknown from here on and the next
 * attempt must be a full repaint. The caller keeps the paint pending, and the
 * outbox drains on POLLOUT between frames; a client that truly went away
 * shows up as a write error there, not here. */
static void update_viewport(conn_t *c, const screen_t *src) {
  screen_follow_cursor(src, c->cols, c->rows, &c->view_x, &c->view_y);
  c->shown_cols = src->cols > c->view_x ? (uint16_t)(src->cols - c->view_x) : 0;
  c->shown_rows = src->rows > c->view_y ? (uint16_t)(src->rows - c->view_y) : 0;
  if (c->shown_cols > c->cols) c->shown_cols = c->cols;
  if (c->shown_rows > c->rows) c->shown_rows = c->rows;
}

/* The multi-client tag, stamped into one client's projected view: this is
 * per-client chrome (every terminal has its own count of "others", its own
 * crop offset), so it cannot live on the shared status bar, which is
 * composed once and might be cropped out of a panned view anyway. Top-right
 * of the client's own screen, which is always visible; toasts keep the
 * bottom-right. Written after projection and before the diff, so it costs
 * bytes only when it changes -- and when it disappears, the diff restores
 * the shared cells beneath it on its own. */
static void stamp_indicator(server_t *s, conn_t *c) {
  if (!app_cfg_attach_indicator()) return;
  size_t viewers = display_count(s);
  if (viewers < 2) return;
  char tag[48];
  if (c->view_x || c->view_y)
    /* The crop's origin in the shared screen: "you are at +col+row of
     * something bigger", which is the disorienting case the tag exists for. */
    snprintf(tag, sizeof tag, " %zu clients +%u+%u ", viewers,
             (unsigned)c->view_x, (unsigned)c->view_y);
  else
    snprintf(tag, sizeof tag, " %zu clients ", viewers);
  size_t len = strlen(tag);
  if (len >= c->view.cols) return; /* a view too narrow to say it in */
  uint16_t x0 = (uint16_t)(c->view.cols - len);
  for (size_t i = 0; i < len; i++)
    screen_put_utf8(&c->view, (uint16_t)(x0 + i), 0, &tag[i], 1, (color_t){0},
                    (color_t){0}, ATTR_DIM | ATTR_INVERSE);
}

static bool push_frame(server_t *s, conn_t *c) {
  uint16_t old_x = c->view_x, old_y = c->view_y;
  uint16_t old_cols = c->shown_cols, old_rows = c->shown_rows;
  update_viewport(c, &s->screen);
  screen_project(&c->view, &s->screen, c->view_x, c->view_y);
  stamp_indicator(s, c);
  screen_render(&c->view);
  if (!c->view.out_len) return true;
  if (!conn_send(c, MSG_OUTPUT, c->view.out, c->view.out_len)) {
    /* The projection never entered this client's stream. Keep mouse mapping
     * tied to the last viewport we did enqueue, then retry this one in full. */
    c->view_x = old_x;
    c->view_y = old_y;
    c->shown_cols = old_cols;
    c->shown_rows = old_rows;
    c->view.force_full = true;
    return false;
  }
  return true;
}

static bool push_clients(server_t *s) {
  /* cmd_exec(), config reloads and canonical resizes request a terminal reset
   * on the canonical screen. It is never rendered directly now, so carry that
   * request into every independently diffed client view and consume it here.
   * A newly attached view starts force_full on its own, including when there
   * were no displays around to receive an earlier request. */
  if (s->screen.force_full) {
    for (size_t i = 0; i < s->nconns; i++)
      if (s->conns[i].display) s->conns[i].view.force_full = true;
    s->screen.force_full = false;
  }

  bool ok = true;
  for (size_t i = 0; i < s->nconns; i++) {
    conn_t *c = &s->conns[i];
    if (!c->display) continue;
    bool frame_ok = push_frame(s, c);
    bool gfx_ok = push_graphics(s, c);
    if (!(frame_ok && gfx_ok)) ok = false;
  }
  return ok;
}

/* Size the canonical screen by policy (size_follows). "active" takes the
 * active client's size; "smallest"/"largest" fold every attached display, so
 * any attach, resize or departure can move the canvas. The active client
 * still supplies the cell pixel size either way: graphics go to terminals,
 * and the active one is the best guess at what a cell is worth. */
static void apply_canvas(server_t *s) {
  conn_t *a = active_conn(s);
  int policy = app_cfg_size_follows();
  uint16_t cols = 0, rows = 0;
  if (policy == SIZE_FOLLOWS_ACTIVE) {
    if (a) {
      cols = a->cols;
      rows = a->rows;
    }
  } else {
    for (size_t i = 0; i < s->nconns; i++) {
      conn_t *c = &s->conns[i];
      if (!c->display || !c->cols || !c->rows) continue;
      if (!cols) {
        cols = c->cols;
        rows = c->rows;
      } else if (policy == SIZE_FOLLOWS_SMALLEST) {
        if (c->cols < cols) cols = c->cols;
        if (c->rows < rows) rows = c->rows;
      } else {
        if (c->cols > cols) cols = c->cols;
        if (c->rows > rows) rows = c->rows;
      }
    }
  }
  if (!cols || !rows) { /* nobody is looking: leave the canvas standing */
    request_paint(s, true);
    return;
  }
  bool changed = cols != s->screen.cols || rows != s->screen.rows;
  uint16_t old_w = 0, old_h = 0;
  app_cell_px(s->app, &old_w, &old_h);
  uint16_t cell_w = a && a->cell_w ? a->cell_w : s->fallback_cell_w;
  uint16_t cell_h = a && a->cell_h ? a->cell_h : s->fallback_cell_h;
  bool px_changed = cell_w != old_w || cell_h != old_h;
  if (changed) {
    screen_resize(&s->screen, cols, rows);
    app_resize(s->app, cols, rows);
  }
  if (px_changed) app_set_cell_px(s->app, cell_w, cell_h);
  if (changed || px_changed) app_compose(s->app, &s->screen);
  request_paint(s, true);
}

static void make_active(server_t *s, conn_t *c, bool new_activity) {
  if (s->active_fd != c->fd) app_cancel_client_interaction(s->app);
  s->active_fd = c->fd;
  if (new_activity) c->activity = ++s->activity_seq;
  apply_canvas(s);
}

static void promote_latest(server_t *s) {
  conn_t *latest = NULL;
  for (size_t i = 0; i < s->nconns; i++) {
    conn_t *c = &s->conns[i];
    if (c->display && (!latest || c->activity > latest->activity)) latest = c;
  }
  app_cancel_client_interaction(s->app);
  if (!latest) {
    s->active_fd = -1;
    request_paint(s, true);
    return;
  }
  s->active_fd = latest->fd;
  apply_canvas(s);
}

/* A non-active departure changes nothing under "active", but under
 * "smallest"/"largest" the canvas may have been sized by the very client
 * that just left. */
static void close_conn(server_t *s, size_t i) {
  if (conn_close(s, i))
    promote_latest(s);
  else
    apply_canvas(s);
}

static void drop_conn(server_t *s, size_t i, uint8_t reason) {
  if (conn_drop(s, i, reason))
    promote_latest(s);
  else
    apply_canvas(s);
}

static void drop_fd(server_t *s, int fd, uint8_t reason) {
  for (size_t i = 0; i < s->nconns; i++)
    if (s->conns[i].fd == fd) {
      drop_conn(s, i, reason);
      return;
    }
}

typedef struct {
  server_t *server;
  conn_t *conn;
  bool activity;
} client_input_t;

static void client_event(const input_event_t *ev, void *ud) {
  client_input_t *ctx = ud;
  server_t *s = ctx->server;
  conn_t *c = ctx->conn;
  bool user =
      ev->kind == EV_KEY || ev->kind == EV_MOUSE || ev->kind == EV_PASTE;
  if (user && !ctx->activity) {
    if (s->active_fd != c->fd) app_cancel_client_interaction(s->app);
    s->active_fd = c->fd;
    c->activity = ++s->activity_seq;
    ctx->activity = true;
  }

  if (ev->kind == EV_MOUSE) {
    /* The event names the frame this client actually saw. Filler has no target;
     * a cropped cell maps back into the canonical screen before hit-testing. */
    if (ev->mx >= c->shown_cols || ev->my >= c->shown_rows) {
      /* Filler has no hit target, but a release there still has to terminate
       * a drag that began over real content. Otherwise the shared app keeps a
       * button logically held until somebody happens to press a key. */
      if (ev->maction == MOUSE_RELEASE) app_cancel_client_pointer(s->app);
      return;
    }
    input_event_t mapped = *ev;
    mapped.mx = (uint16_t)(mapped.mx + c->view_x);
    mapped.my = (uint16_t)(mapped.my + c->view_y);
    app_event(s->app, &mapped);
    return;
  }
  app_event(s->app, ev);
}

static bool feed_client(server_t *s, conn_t *c, const uint8_t *data, size_t len,
                        bool timeout) {
  client_input_t ctx = {.server = s, .conn = c};
  if (timeout)
    input_timeout(c->in, client_event, &ctx);
  else
    input_feed(c->in, data, len, client_event, &ctx);
  if (ctx.activity) apply_canvas(s);
  if (input_pending(c->in)) c->esc_due = now_ms() + ESC_MS;
  return ctx.activity;
}

static void queue_detach(int fds[MAX_CONNS], size_t *n, int fd) {
  if (fd < 0) return;
  for (size_t i = 0; i < *n; i++)
    if (fds[i] == fd) return;
  if (*n < MAX_CONNS) fds[(*n)++] = fd;
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
#ifdef _WIN32
  /* The pump thread owns the handles, so dropping a watch is a message rather
   * than a close: clear the set here and the adds below republish it. */
  sl_watch_clear(w->fd);
#elif defined(__APPLE__)
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
    char dirbuf[512], base[128], dir[512];
    snprintf(dir, sizeof dir, "%s", path_dir(files[i], dirbuf, sizeof dirbuf));
    snprintf(base, sizeof base, "%s", path_base(files[i]));
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
#elif defined(_WIN32)
    /* Watch the directory and let the debounced re-read decide, which is the
     * same conclusion the kqueue branch reaches for the same reason: an editor
     * that saves by rename would slip out from under a watch on the file. */
    sl_watch_add(w->fd, dir);
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

/* Only inotify reports which file changed; the kqueue and Windows watchers
 * both re-read and let the debounce decide. */
#if !defined(__APPLE__) && !defined(_WIN32)
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
#ifdef _WIN32
  touched = sl_watch_drain(w->fd) != 0;
#elif defined(__APPLE__)
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
#ifdef _WIN32
  if (sl_self_exe(self, sizeof self)) setenv("SLOSH_BIN", self, 1);
#elif defined(__APPLE__)
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

#ifdef _WIN32
  /* Windows delivers Ctrl-C and shutdown through a console control handler
   * rather than as signals, and has no SIGPIPE or SIGCHLD to arrange for. */
  signal(SIGINT, on_sig);
  signal(SIGTERM, on_sig);
#else
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
#endif

  server_t s = {.active_fd = -1};
  s.app = app_new(argv, cols, rows);
  if (s.app) app_set_session(s.app, name);
  if (!s.app) return 1;
  app_cell_px(s.app, &s.fallback_cell_w, &s.fallback_cell_h);
  if (layout) {
    char err[256] = {0};
    if (!app_apply_layout_file(s.app, layout, true, err, sizeof err))
      fprintf(stderr, "slosh: %s: %s\n", layout, err[0] ? err : "bad layout");
  }
  app_resize(s.app, cols, rows);
  screen_init(&s.screen, cols, rows);
  s.cmd_in = input_new();

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
#ifdef _WIN32
    inofd = sl_watch_init();
#elif defined(__APPLE__)
    inofd = kqueue();
    if (inofd >= 0) fcntl(inofd, F_SETFD, FD_CLOEXEC);
#else
    inofd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    watches.fd = inofd;
    watch_config(&watches);
  }

  s.pending_paint = true;
  s.next_frame = now_ms();

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
    if (s.pending_paint) {
      int64_t due = s.next_frame - now_ms();
      timeout = due <= 0 ? 0 : (int)due;
    }
    for (size_t i = 0; i < s.nconns; i++)
      if (s.conns[i].display && input_pending(s.conns[i].in)) {
        int64_t due = s.conns[i].esc_due - now_ms();
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
      s.pending_paint = true;
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

    int detach_fds[MAX_CONNS];
    size_t ndetach = 0;
    for (size_t ci = 0; ci < s.nconns;) {
      struct pollfd *pf = &pfds[conn_slot + ci];
      if (pf->fd != s.conns[ci].fd) {
        ci++;
        continue;
      }
      if (pf->revents & POLLOUT) {
        if (!conn_flush(&s.conns[ci])) {
          close_conn(&s,
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
        close_conn(&s, ci); /* gone: the session keeps running regardless */
        continue;
      }
      msg_reader_feed(&s.conns[ci].reader, buf, (size_t)got);
      msg_t m;
      bool closed = false;
      while (!closed && msg_reader_next(&s.conns[ci].reader, &m)) {
        switch (m.type) {
        case MSG_HELLO:
        case MSG_RESIZE: {
          conn_t *c = &s.conns[ci];
          bool fresh = m.type == MSG_HELLO && !c->display;
          bool first_viewer = fresh && display_count(&s) == 0;
          if (!c->display && !fresh) break;

          /* multi_attach false is the classic rule: the new client displaces
           * every display before it (EXIT_REPLACED, which old clients already
           * understand). Dropping compacts the array, so the index -- and the
           * pointer taken from it -- are re-derived afterwards. */
          if (fresh && !app_cfg_multi_attach()) {
            for (size_t j = 0; j < s.nconns;) {
              if (j != ci && s.conns[j].display) {
                conn_drop(&s, j, EXIT_REPLACED);
                if (j < ci) ci--; /* the array compacted under us */
              } else
                j++;
            }
            c = &s.conns[ci];
            first_viewer = display_count(&s) == 0;
          }

          uint16_t cols = c->cols ? c->cols : s.screen.cols;
          uint16_t rows = c->rows ? c->rows : s.screen.rows;
          if (m.len >= 4) {
            uint16_t got_cols = (uint16_t)(m.data[0] << 8 | m.data[1]);
            uint16_t got_rows = (uint16_t)(m.data[2] << 8 | m.data[3]);
            if (got_cols && got_rows) {
              cols = got_cols;
              rows = got_rows;
            }
          }
          uint16_t cell_w = c->cell_w, cell_h = c->cell_h;
          if (m.len >= 8) {
            cell_w = (uint16_t)(m.data[4] << 8 | m.data[5]);
            cell_h = (uint16_t)(m.data[6] << 8 | m.data[7]);
          }

          if (fresh) {
            c->display = true;
            c->cols = cols;
            c->rows = rows;
            c->cell_w = cell_w;
            c->cell_h = cell_h;
            c->in = input_new();
            c->gfx = gfx_new();
            screen_init(&c->view, cols, rows);
            c->view_init = true;
            make_active(&s, c, true);
            if (first_viewer) {
              app_splash(s.app);
              app_compose(s.app, &s.screen);
            }
          } else {
            bool local_resize = cols != c->cols || rows != c->rows;
            c->cols = cols;
            c->rows = rows;
            c->cell_w = cell_w;
            c->cell_h = cell_h;
            if (local_resize) screen_resize(&c->view, cols, rows);
            /* Unconditional: under "active" a non-active resize changes
             * nothing (apply_canvas reads the active client), but under
             * "smallest"/"largest" every display's size is a vote. */
            apply_canvas(&s);
          }
          update_viewport(c, &s.screen);
          request_paint(&s, true);
          break;
        }
        case MSG_INPUT:
          if (!s.conns[ci].display) break; /* only display clients type */
          feed_client(&s, &s.conns[ci], m.data, m.len, false);
          /* A selection copy belongs to the terminal whose mouse released it,
           * even if another client types before this poll cycle is painted. */
          push_clipboard_to(&s, &s.conns[ci]);
          if (app_detach_requested(s.app)) {
            app_clear_detach(s.app);
            queue_detach(detach_fds, &ndetach, s.conns[ci].fd);
          }
          request_paint(&s, true);
          break;
        case MSG_DETACH:
          drop_conn(&s, ci, EXIT_DETACHED);
          closed = true;
          break;
        case MSG_CMD: {
          /* The same vocabulary the headless driver speaks (cmd.c), so a
             * script written against one works against the other. */
          bool q = false;
          char *reply =
              cmd_exec(s.app, &s.screen, s.cmd_in, (const char *)m.data, &q);
          const char *body = reply ? reply : "{\"error\":\"unknown command\"}";
          conn_send(&s.conns[ci], MSG_REPLY, body, strlen(body));
          free(reply);
          if (q) g_stop = 1;
          s.screen.force_full = true; /* it may have changed the layout */
          if (app_detach_requested(s.app)) {
            app_clear_detach(s.app);
            queue_detach(detach_fds, &ndetach, s.active_fd);
          }
          request_paint(&s, true);
          break;
        }
        default: break;
        }
      }
      if (!closed) ci++;
    }

    for (size_t i = 0; i < s.nconns; i++) {
      conn_t *c = &s.conns[i];
      if (c->display && input_pending(c->in) && now_ms() >= c->esc_due) {
        feed_client(&s, c, NULL, 0, true);
        push_clipboard_to(&s, c);
        if (app_detach_requested(s.app)) {
          app_clear_detach(s.app);
          queue_detach(detach_fds, &ndetach, c->fd);
        }
        request_paint(&s, true);
      }
    }
    for (size_t i = 0; i < ndetach; i++)
      drop_fd(&s, detach_fds[i], EXIT_DETACHED);

    for (size_t i = 0; i < npanes; i++) {
      if (pfds[pane_slot + i].revents & (POLLIN | POLLHUP)) {
        app_pump_fd(s.app, pfds[pane_slot + i].fd);
        if (!s.pending_paint) {
          s.pending_paint = true;
          s.next_frame = now_ms() + FRAME_MS;
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
        /* The knobs this loop lives by may have moved. multi_attach turned
         * off evicts every display but the active one -- the reloaded config
         * says one client, so one client it is -- and a changed size_follows
         * re-sizes the canvas to the policy now in force. */
        if (!app_cfg_multi_attach()) {
          for (size_t i = 0; i < s.nconns;) {
            if (s.conns[i].display && s.conns[i].fd != s.active_fd)
              drop_conn(&s, i, EXIT_REPLACED);
            else
              i++;
          }
        }
        apply_canvas(&s);
      } else {
        app_toast(s.app, err[0] ? err : "config reload failed");
      }
      s.screen.force_full = true;
      request_paint(&s, true);
    }

    app_reap(s.app);

    /* The event-less work: a selection drag held past a pane's edge scrolls
     * on a clock (app_next_deadline_ms is what woke the poll for it), and a
     * step that moved anything is a frame the client is owed. */
    if (app_tick(s.app) && !s.pending_paint) {
      s.pending_paint = true;
      s.next_frame = now_ms();
    }

    /* A detach raised by a non-display control command belongs to the active
     * display. Display input records its own fd above before another client can
     * become active. */
    if (app_detach_requested(s.app)) {
      app_clear_detach(s.app);
      drop_fd(&s, s.active_fd, EXIT_DETACHED);
    }

    if (s.pending_paint && now_ms() >= s.next_frame) {
      app_compose(s.app, &s.screen);
      /* Each client remembers its own full repaint and image transmissions.
       * One full outbox keeps a retry pending without holding anybody else up. */
      bool clients_ok = push_clients(&s);
      push_clipboard(&s);
      s.pending_paint = !clients_ok;
      s.next_frame = now_ms() + (s.pending_paint ? FRAME_MS : 0);
    }
  }

  while (s.nconns) conn_drop(&s, 0, EXIT_SESSION_ENDED);
  unlink(path);
  close(lfd);
  input_free(s.cmd_in);
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

#ifdef _WIN32
  /* No fork: the daemon is started as a fresh detached process rather than as
   * a copy of this one, reconstructing on its command line what the forked
   * child would have inherited. `--server` is the flag main.c uses to run
   * server_run() directly instead of spawning again. */
  {
    char self[512];
    if (!sl_self_exe(self, sizeof self)) return -1;
    const char *av[16];
    char colbuf[16], rowbuf[16];
    snprintf(colbuf, sizeof colbuf, "%u", (unsigned)cols);
    snprintf(rowbuf, sizeof rowbuf, "%u", (unsigned)rows);
    size_t n = 0;
    av[n++] = self;
    av[n++] = "--server";
    av[n++] = "-s";
    av[n++] = name;
    av[n++] = "--cols";
    av[n++] = colbuf;
    av[n++] = "--rows";
    av[n++] = rowbuf;
    if (layout) {
      av[n++] = "--layout";
      av[n++] = layout;
    }
    if (!watch) av[n++] = "--no-reload";
    if (argv && argv[0]) {
      av[n++] = "--";
      for (int i = 0; argv[i] && n < 15; i++) av[n++] = argv[i];
    }
    av[n] = NULL;
    if (sl_spawn_detached(av, logp) < 0) return -1;
  }
#else
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
#endif

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
