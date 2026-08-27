#define _GNU_SOURCE
#include "proto.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define HDR MSG_HDR

void msg_reader_init(msg_reader_t *r) {
  memset(r, 0, sizeof *r);
  r->cap = 65536;
  r->buf = malloc(r->cap);
}

void msg_reader_free(msg_reader_t *r) {
  free(r->buf);
  free(r->msg);
  memset(r, 0, sizeof *r);
}

void msg_reader_feed(msg_reader_t *r, const uint8_t *data, size_t len) {
  if (r->len + len > r->cap) {
    while (r->cap < r->len + len) r->cap *= 2;
    r->buf = realloc(r->buf, r->cap);
  }
  memcpy(r->buf + r->len, data, len);
  r->len += len;
}

bool msg_reader_next(msg_reader_t *r, msg_t *out) {
  if (r->len < HDR) return false;
  uint32_t len = (uint32_t)r->buf[1] << 24 | (uint32_t)r->buf[2] << 16 |
                 (uint32_t)r->buf[3] << 8 | r->buf[4];
  if (r->len < HDR + (size_t)len) return false;

  /* Lift the payload out before compacting the buffer, so the caller's pointer
   * does not alias bytes we are about to move. Valid until the next call. */
  if (len + 1 > r->msg_cap) {
    r->msg_cap = len + 1;
    r->msg = realloc(r->msg, r->msg_cap);
  }
  if (len) memcpy(r->msg, r->buf + HDR, len);
  r->msg[len] = 0; /* JSON payloads want a terminator */

  out->type = r->buf[0];
  out->len = len;
  out->data = r->msg;

  size_t total = HDR + (size_t)len;
  memmove(r->buf, r->buf + total, r->len - total);
  r->len -= total;
  return true;
}

int msg_send(int fd, uint8_t type, const void *data, size_t len) {
  uint8_t hdr[HDR] = {type, (uint8_t)(len >> 24), (uint8_t)(len >> 16),
                      (uint8_t)(len >> 8), (uint8_t)len};
#ifdef _WIN32
  /* No sendmsg: the two buffers are simply sent in order. The gather was only
   * ever an optimisation -- what matters is that the header and its payload
   * reach the socket as one contiguous stream, which sequential sends on a
   * stream socket also guarantee. */
  const void *parts[2] = {hdr, data};
  size_t lens[2] = {HDR, len};
  for (int i = 0; i < 2; i++) {
    size_t off = 0;
    while (off < lens[i]) {
      ssize_t w = sl_write(fd, (const char *)parts[i] + off, lens[i] - off);
      if (w < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return -1;
      }
      off += (size_t)w;
    }
  }
  return 0;
#else
  struct iovec iov[2] = {{hdr, HDR}, {(void *)data, len}};
  size_t total = HDR + len, sent = 0;
  while (sent < total) {
    struct msghdr mh = {0};
    struct iovec cur[2];
    size_t n = 0;
    size_t off = sent;
    for (int i = 0; i < 2; i++) {
      size_t l = iov[i].iov_len;
      if (off >= l) {
        off -= l;
        continue;
      }
      cur[n].iov_base = (char *)iov[i].iov_base + off;
      cur[n].iov_len = l - off;
      off = 0;
      n++;
    }
    mh.msg_iov = cur;
    mh.msg_iovlen = n;
    ssize_t w = sendmsg(fd, &mh, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    sent += (size_t)w;
  }
  return 0;
#endif
}

static int session_dir(char *out, size_t cap) {
#ifdef _WIN32
  /* Per-user and non-roaming, which is what LOCALAPPDATA is for: a session
   * socket is machine-local by definition and must not follow a roaming
   * profile to another host. */
  const char *base = getenv("LOCALAPPDATA");
  if (!base || !*base) base = getenv("TEMP");
  if (!base || !*base) return -1;
  snprintf(out, cap, "%s/slosh", base);
  for (char *p = out; *p; p++)
    if (*p == '\\') *p = '/';
#else
  const char *run = getenv("XDG_RUNTIME_DIR");
  if (run && *run)
    snprintf(out, cap, "%s/slosh", run);
  else
    snprintf(out, cap, "/tmp/slosh-%u", (unsigned)getuid());
#endif
  if (mkdir(out, 0700) != 0 && errno != EEXIST) return -1;
  return 0;
}

int session_socket_path(const char *name, char *out, size_t cap) {
  char dir[512];
  if (session_dir(dir, sizeof dir) != 0) return -1;
  snprintf(out, cap, "%s/%s.sock", dir, name);
  return 0;
}

int session_log_path(const char *name, char *out, size_t cap) {
  char dir[512];
  if (session_dir(dir, sizeof dir) != 0) return -1;
  snprintf(out, cap, "%s/%s.log", dir, name);
  return 0;
}

size_t session_list(char ***out_names) {
  char dir[512];
  if (session_dir(dir, sizeof dir) != 0) return 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  char **names = NULL;
  size_t n = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    size_t l = strlen(e->d_name);
    if (l < 6 || strcmp(e->d_name + l - 5, ".sock") != 0) continue;
    names = realloc(names, (n + 1) * sizeof *names);
    names[n] = strndup(e->d_name, l - 5);
    n++;
  }
  closedir(d);
  *out_names = names;
  return n;
}
