/* Windows implementations of the POSIX surface slosh uses.
 * See include/compat_win.h for why this exists and how fds are tagged. */
#ifdef _WIN32

#include "compat_win.h"

#include <shlwapi.h>
#include <tlhelp32.h>

/* ---- one-time winsock start --------------------------------------------- */

static void wsa_once(void) {
  static LONG done = 0;
  if (InterlockedCompareExchange(&done, 1, 0) == 0) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
  }
}

/* ---- socket table -------------------------------------------------------- */

/* Tagged fds: a slot index, not a SOCKET value. Slots are reused, so a long
 * session that opens and closes panes all day does not walk off the end. */
#define SL_SOCK_MAX 512
static SOCKET g_socks[SL_SOCK_MAX];
static CRITICAL_SECTION g_sock_lock;
static LONG g_sock_init = 0;

static void sock_lock_once(void) {
  if (InterlockedCompareExchange(&g_sock_init, 1, 0) == 0) {
    InitializeCriticalSection(&g_sock_lock);
    for (int i = 0; i < SL_SOCK_MAX; i++) g_socks[i] = INVALID_SOCKET;
    InterlockedExchange(&g_sock_init, 2);
  }
  while (InterlockedCompareExchange(&g_sock_init, 2, 2) != 2) Sleep(0);
}

int sl_sock_wrap(SOCKET s) {
  if (s == INVALID_SOCKET) return -1;
  sock_lock_once();
  EnterCriticalSection(&g_sock_lock);
  int slot = -1;
  for (int i = 0; i < SL_SOCK_MAX; i++) {
    if (g_socks[i] == INVALID_SOCKET) {
      g_socks[i] = s;
      slot = i;
      break;
    }
  }
  LeaveCriticalSection(&g_sock_lock);
  if (slot < 0) {
    closesocket(s);
    return -1;
  }
  return SL_SOCK_BASE + slot;
}

SOCKET sl_sock_get(int fd) {
  if (!sl_is_sock(fd)) return INVALID_SOCKET;
  int slot = fd - SL_SOCK_BASE;
  if (slot < 0 || slot >= SL_SOCK_MAX) return INVALID_SOCKET;
  sock_lock_once();
  EnterCriticalSection(&g_sock_lock);
  SOCKET s = g_socks[slot];
  LeaveCriticalSection(&g_sock_lock);
  return s;
}

static void sock_release(int fd) {
  int slot = fd - SL_SOCK_BASE;
  if (slot < 0 || slot >= SL_SOCK_MAX) return;
  sock_lock_once();
  EnterCriticalSection(&g_sock_lock);
  g_socks[slot] = INVALID_SOCKET;
  LeaveCriticalSection(&g_sock_lock);
}

/* ---- socketpair ---------------------------------------------------------- */

/* Windows has no socketpair(). A listener bound to 127.0.0.1:0 that accepts
 * exactly one connection is the standard stand-in, and is what the pump
 * threads below hang their output on. */
static int raw_socketpair(SOCKET out[2]) {
  wsa_once();
  out[0] = out[1] = INVALID_SOCKET;
  SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (l == INVALID_SOCKET) return -1;

  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  int alen = (int)sizeof a;
  if (bind(l, (struct sockaddr *)&a, alen) != 0 || listen(l, 1) != 0 ||
      getsockname(l, (struct sockaddr *)&a, &alen) != 0) {
    closesocket(l);
    return -1;
  }

  SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (c == INVALID_SOCKET) {
    closesocket(l);
    return -1;
  }
  if (connect(c, (struct sockaddr *)&a, alen) != 0) {
    closesocket(c);
    closesocket(l);
    return -1;
  }
  SOCKET s = accept(l, NULL, NULL);
  closesocket(l);
  if (s == INVALID_SOCKET) {
    closesocket(c);
    return -1;
  }
  /* Nagle would add 40ms to every keystroke round trip. */
  BOOL yes = TRUE;
  setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
  out[0] = c;
  out[1] = s;
  return 0;
}

int sl_socketpair(int fds[2]) {
  SOCKET s[2];
  if (raw_socketpair(s) != 0) return -1;
  fds[0] = sl_sock_wrap(s[0]);
  fds[1] = sl_sock_wrap(s[1]);
  if (fds[0] < 0 || fds[1] < 0) {
    if (fds[0] >= 0)
      sl_close(fds[0]);
    else
      closesocket(s[0]);
    if (fds[1] >= 0)
      sl_close(fds[1]);
    else
      closesocket(s[1]);
    return -1;
  }
  return 0;
}

int sl_socket(int domain, int type, int proto) {
  wsa_once();
  SOCKET s = socket(domain, type, proto);
  if (s == INVALID_SOCKET) {
    errno = EIO;
    return -1;
  }
  return sl_sock_wrap(s);
}

int sl_bind(int fd, const struct sockaddr *a, int len) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) return -1;
  if (bind(s, a, len) != 0) {
    /* WSAEADDRINUSE is what a leftover socket file looks like here; report it
     * as EEXIST so the caller's existing message stays true. */
    int e = WSAGetLastError();
    errno = (e == WSAEADDRINUSE) ? EEXIST : EIO;
    return -1;
  }
  return 0;
}

int sl_listen(int fd, int backlog) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) return -1;
  return listen(s, backlog) == 0 ? 0 : -1;
}

int sl_accept(int fd, struct sockaddr *a, int *len) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) return -1;
  SOCKET c = accept(s, a, len);
  if (c == INVALID_SOCKET) {
    int e = WSAGetLastError();
    errno = (e == WSAEWOULDBLOCK) ? EAGAIN : EIO;
    return -1;
  }
  return sl_sock_wrap(c);
}

int sl_connect(int fd, const struct sockaddr *a, int len) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) return -1;
  if (connect(s, a, len) != 0) {
    errno = ECONNREFUSED;
    return -1;
  }
  return 0;
}

long sl_send(int fd, const void *b, size_t n, int flags) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) {
    errno = EBADF;
    return -1;
  }
  int r = send(s, (const char *)b, (int)n, flags);
  if (r < 0) {
    int e = WSAGetLastError();
    errno = (e == WSAEWOULDBLOCK) ? EAGAIN : EPIPE;
    return -1;
  }
  return r;
}

long sl_recv(int fd, void *b, size_t n, int flags) {
  SOCKET s = sl_sock_get(fd);
  if (s == INVALID_SOCKET) {
    errno = EBADF;
    return -1;
  }
  int r = recv(s, (char *)b, (int)n, flags);
  if (r < 0) {
    int e = WSAGetLastError();
    errno = (e == WSAEWOULDBLOCK) ? EAGAIN : EIO;
    return -1;
  }
  return r;
}

int sl_unlink(const char *path) {
  if (DeleteFileA(path)) return 0;
  DWORD e = GetLastError();
  if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
    errno = ENOENT;
    return -1;
  }
  errno = EACCES;
  return -1;
}

int sl_socketpair_pump(SOCKET *inner, int *outer_fd) {
  SOCKET s[2];
  if (raw_socketpair(s) != 0) return -1;
  int fd = sl_sock_wrap(s[1]);
  if (fd < 0) {
    closesocket(s[0]);
    closesocket(s[1]);
    return -1;
  }
  *inner = s[0];
  *outer_fd = fd;
  return 0;
}

/* ---- read / write / close ------------------------------------------------ */

long sl_read(int fd, void *buf, size_t n) {
  if (sl_is_sock(fd)) {
    SOCKET s = sl_sock_get(fd);
    if (s == INVALID_SOCKET) {
      errno = EBADF;
      return -1;
    }
    int r = recv(s, (char *)buf, (int)n, 0);
    if (r < 0) {
      int e = WSAGetLastError();
      errno = (e == WSAEWOULDBLOCK) ? EAGAIN : EIO;
      return -1;
    }
    return r;
  }
  return _read(fd, buf, (unsigned)n);
}

long sl_write(int fd, const void *buf, size_t n) {
  if (sl_is_sock(fd)) {
    SOCKET s = sl_sock_get(fd);
    if (s == INVALID_SOCKET) {
      errno = EBADF;
      return -1;
    }
    int r = send(s, (const char *)buf, (int)n, 0);
    if (r < 0) {
      int e = WSAGetLastError();
      errno = (e == WSAEWOULDBLOCK) ? EAGAIN : EPIPE;
      return -1;
    }
    return r;
  }
  return _write(fd, buf, (unsigned)n);
}

int sl_close(int fd) {
  if (sl_is_sock(fd)) {
    SOCKET s = sl_sock_get(fd);
    sock_release(fd);
    if (s != INVALID_SOCKET) closesocket(s);
    return 0;
  }
  if (fd < 0) return 0;
  return _close(fd);
}

/* ---- poll ---------------------------------------------------------------- */

int sl_poll(struct sl_pollfd *fds, nfds_t n, int timeout_ms) {
  wsa_once();
  WSAPOLLFD wp[256];
  if (n > 256) n = 256;

  nfds_t m = 0;
  int map[256];
  for (nfds_t i = 0; i < n; i++) {
    fds[i].revents = 0;
    SOCKET s = sl_sock_get(fds[i].fd);
    if (s == INVALID_SOCKET) continue; /* non-socket: never ready */
    wp[m].fd = s;
    wp[m].events = 0;
    if (fds[i].events & POLLIN) wp[m].events |= POLLRDNORM;
    if (fds[i].events & POLLOUT) wp[m].events |= POLLWRNORM;
    map[m] = (int)i;
    m++;
  }
  if (m == 0) {
    if (timeout_ms > 0) Sleep((DWORD)timeout_ms);
    return 0;
  }

  int r = WSAPoll(wp, (ULONG)m, timeout_ms);
  if (r < 0) {
    errno = EIO;
    return -1;
  }
  int ready = 0;
  for (nfds_t i = 0; i < m; i++) {
    short re = 0;
    if (wp[i].revents & (POLLRDNORM | POLLIN)) re |= POLLIN;
    if (wp[i].revents & (POLLWRNORM | POLLOUT)) re |= POLLOUT;
    if (wp[i].revents & POLLERR) re |= POLLERR;
    if (wp[i].revents & POLLHUP) re |= POLLHUP;
    if (wp[i].revents & POLLNVAL) re |= POLLNVAL;
    if (re) {
      fds[map[i]].revents = re;
      ready++;
    }
  }
  return ready;
}

/* ---- fcntl / ioctl ------------------------------------------------------- */

int sl_fcntl(int fd, int cmd, ...) {
  if (cmd == F_GETFL || cmd == F_GETFD) return 0;
  if (cmd == F_SETFD) return 0; /* CLOEXEC: handles are not inherited anyway */
  if (cmd == F_SETFL) {
    va_list ap;
    va_start(ap, cmd);
    int flags = va_arg(ap, int);
    va_end(ap);
    if (sl_is_sock(fd)) {
      SOCKET s = sl_sock_get(fd);
      if (s == INVALID_SOCKET) return -1;
      u_long nb = (flags & O_NONBLOCK) ? 1 : 0;
      return ioctlsocket(s, FIONBIO, &nb) == 0 ? 0 : -1;
    }
    return 0;
  }
  return 0;
}

/* Window size lives on the console, not on the fd; TIOCSWINSZ is the ConPTY's
 * job and is handled in pty_win.c, so here it is a no-op that reports success
 * rather than an error the caller would log every resize. */
int sl_ioctl(int fd, unsigned long req, ...) {
  if (req == TIOCGWINSZ) {
    va_list ap;
    va_start(ap, req);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);
    if (!ws) return -1;
    CONSOLE_SCREEN_BUFFER_INFO ci;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &ci)) {
      ws->ws_col = (unsigned short)(ci.srWindow.Right - ci.srWindow.Left + 1);
      ws->ws_row = (unsigned short)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
      ws->ws_xpixel = 0;
      ws->ws_ypixel = 0;
      return 0;
    }
    return -1;
  }
  return 0;
}

/* ---- small libc gaps ----------------------------------------------------- */

char *sl_strndup(const char *s, size_t n) {
  if (!s) return NULL;
  size_t len = 0;
  while (len < n && s[len]) len++;
  char *p = (char *)malloc(len + 1);
  if (!p) return NULL;
  memcpy(p, s, len);
  p[len] = 0;
  return p;
}

/* Normalises separators to '/' on the way out: the rest of slosh compares and
 * splits paths on '/', and a mix of the two would make two spellings of one
 * directory look like two directories. */
char *sl_realpath(const char *path, char *resolved) {
  if (!path) return NULL;
  char *out = resolved ? resolved : (char *)malloc(PATH_MAX);
  if (!out) return NULL;
  DWORD n = GetFullPathNameA(path, PATH_MAX, out, NULL);
  if (n == 0 || n >= PATH_MAX) {
    if (!resolved) free(out);
    return NULL;
  }
  if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES) {
    if (!resolved) free(out);
    return NULL;
  }
  for (char *p = out; *p; p++)
    if (*p == '\\') *p = '/';
  return out;
}

long sl_readlink(const char *path, char *buf, size_t cap) {
  (void)path;
  (void)buf;
  (void)cap;
  errno = EINVAL; /* no /proc to chase; callers fall back */
  return -1;
}

int sl_setenv(const char *k, const char *v, int overwrite) {
  if (!overwrite && getenv(k)) return 0;
  return _putenv_s(k, v ? v : "") == 0 ? 0 : -1;
}
int sl_unsetenv(const char *k) { return _putenv_s(k, "") == 0 ? 0 : -1; }
int sl_nanosleep_ms(long ms) {
  Sleep((DWORD)ms);
  return 0;
}

/* ---- processes ----------------------------------------------------------- */

int sl_kill(pid_t pid, int sig) {
  if (pid <= 0) return -1;
  HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, (DWORD)pid);
  if (!h) return -1;
  int r = 0;
  if (sig == 0) {
    r = 0; /* existence probe */
  } else if (!TerminateProcess(h, 1)) {
    r = -1;
  }
  CloseHandle(h);
  return r;
}

pid_t sl_waitpid(pid_t pid, int *status, int options) {
  if (pid <= 0) return -1;
  HANDLE h =
      OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, (DWORD)pid);
  if (!h) {
    if (status) *status = 0;
    return pid; /* already gone and reaped */
  }
  DWORD w = WaitForSingleObject(h, (options & WNOHANG) ? 0 : INFINITE);
  if (w == WAIT_TIMEOUT) {
    CloseHandle(h);
    return 0;
  }
  DWORD code = 0;
  GetExitCodeProcess(h, &code);
  CloseHandle(h);
  if (status) *status = (int)((code & 0xff) << 8);
  return pid;
}

bool sl_proc_cmdline(pid_t pid, char *out, size_t cap) {
  /* The image name is what a pane title needs, and it is available without
   * the remote-memory read a full command line would require. */
  if (pid <= 0 || !out || cap == 0) return false;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32 pe = {.dwSize = sizeof pe};
  bool found = false;
  if (Process32First(snap, &pe)) {
    do {
      if (pe.th32ProcessID == (DWORD)pid) {
        snprintf(out, cap, "%s", pe.szExeFile);
        found = true;
        break;
      }
    } while (Process32Next(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

pid_t sl_proc_foreground(pid_t root) {
  if (root <= 0) return -1;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return -1;

  /* Walk down: the shell's child, then that child's child, and so on. A depth
   * cap keeps a cycle in a corrupt snapshot from spinning. */
  DWORD cur = (DWORD)root;
  for (int depth = 0; depth < 16; depth++) {
    PROCESSENTRY32 pe = {.dwSize = sizeof pe};
    DWORD child = 0;
    if (Process32First(snap, &pe)) {
      do {
        if (pe.th32ParentProcessID == cur && pe.th32ProcessID != cur)
          child = pe.th32ProcessID; /* last wins: the most recent */
      } while (Process32Next(snap, &pe));
    }
    if (!child) break;
    cur = child;
  }
  CloseHandle(snap);
  return cur == (DWORD)root ? -1 : (pid_t)cur;
}

bool sl_self_exe(char *out, size_t cap) {
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
  if (n == 0 || n >= cap) return false;
  for (char *p = out; *p; p++)
    if (*p == '\\') *p = '/';
  return true;
}

bool sl_which(const char *exe) {
  if (!exe || !*exe) return false;
  char found[PATH_MAX];
  /* SearchPath appends only the one extension it is given, so the executable
   * suffixes are tried in turn -- an editor installed as a .cmd shim (which is
   * how several arrive on Windows) is on PATH just as much as a .exe. A name
   * that already carries an extension is looked up as written. */
  if (strchr(exe, '.')) {
    DWORD n = SearchPathA(NULL, exe, NULL, (DWORD)sizeof found, found, NULL);
    return n > 0 && n < sizeof found;
  }
  static const char *const exts[] = {".exe", ".cmd", ".bat", ".com", NULL};
  for (int i = 0; exts[i]; i++) {
    DWORD n = SearchPathA(NULL, exe, exts[i], (DWORD)sizeof found, found, NULL);
    if (n > 0 && n < sizeof found) return true;
  }
  return false;
}

const char *sl_path_native(const char *in, char *out, size_t cap) {
  if (!in || !out || cap == 0) return in;
  /* GetFullPathName canonicalises and normalises the separators in one step;
   * the manual pass is the fallback for a path it will not take. */
  DWORD n = GetFullPathNameA(in, (DWORD)cap, out, NULL);
  if (n == 0 || n >= cap) {
    snprintf(out, cap, "%s", in);
    for (char *p = out; *p; p++)
      if (*p == '/') *p = '\\';
    return out;
  }
  for (char *p = out; *p; p++)
    if (*p == '/') *p = '\\';
  return out;
}

int sl_spawn_detached(const char *const argv[], const char *logpath) {
  /* fork() has no Windows equivalent, so the daemon is started as a fresh
   * detached process rather than as a copy of this one. Everything the server
   * needs is already on its command line, which is what makes that possible. */
  char cmd[8192];
  size_t off = 0;
  for (int i = 0; argv[i]; i++) {
    int need_q = strchr(argv[i], ' ') != NULL;
    off +=
        (size_t)snprintf(cmd + off, sizeof cmd - off, "%s%s%s%s", i ? " " : "",
                         need_q ? "\"" : "", argv[i], need_q ? "\"" : "");
    if (off >= sizeof cmd) return -1;
  }

  SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
  HANDLE log = INVALID_HANDLE_VALUE;
  if (logpath) {
    log = CreateFileA(logpath, FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, NULL);
  }

  STARTUPINFOA si = {.cb = sizeof si};
  if (log != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = log;
    si.hStdError = log;
  }
  PROCESS_INFORMATION pi = {0};
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, log != INVALID_HANDLE_VALUE,
                           CREATE_NEW_PROCESS_GROUP | DETACHED_PROCESS, NULL,
                           NULL, &si, &pi);
  if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
  if (!ok) return -1;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return (int)pi.dwProcessId;
}

/* ---- console ------------------------------------------------------------- */

static DWORD g_in_mode = 0, g_out_mode = 0;
static bool g_console_saved = false;

bool sl_console_raw(void) {
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (in == INVALID_HANDLE_VALUE || out == INVALID_HANDLE_VALUE) return false;
  if (!GetConsoleMode(in, &g_in_mode) || !GetConsoleMode(out, &g_out_mode))
    return false;
  g_console_saved = true;

  /* VIRTUAL_TERMINAL_INPUT makes conhost hand us the same escape sequences a
   * POSIX tty would, so input.c needs no Windows-specific decoding. */
  DWORD nin = g_in_mode;
  nin &=
      (DWORD) ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
  nin |= ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT;
  SetConsoleMode(in, nin);

  DWORD nout = g_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
               DISABLE_NEWLINE_AUTO_RETURN;
  SetConsoleMode(out, nout);
  SetConsoleOutputCP(CP_UTF8);
  return true;
}

void sl_console_restore(void) {
  if (!g_console_saved) return;
  SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), g_in_mode);
  SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_out_mode);
  g_console_saved = false;
}

static volatile LONG g_resized = 0;

static unsigned __stdcall stdin_pump(void *arg) {
  SOCKET s = (SOCKET)(uintptr_t)arg;
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  for (;;) {
    /* ReadFile on the console handle returns the VT bytes that
     * ENABLE_VIRTUAL_TERMINAL_INPUT produces. Window-resize records are not
     * bytes, so they are noticed separately via the size poll in the client. */
    char buf[4096];
    DWORD got = 0;
    if (!ReadFile(in, buf, sizeof buf, &got, NULL) || got == 0) break;
    size_t off = 0;
    while (off < got) {
      int w = send(s, buf + off, (int)(got - off), 0);
      if (w <= 0) goto done;
      off += (size_t)w;
    }
  }
done:
  shutdown(s, SD_SEND);
  return 0;
}

int sl_console_stdin_socket(void) {
  SOCKET sp[2];
  if (raw_socketpair(sp) != 0) return -1;
  /* sp[0] is written by the pump, sp[1] is what the caller polls. */
  HANDLE t = (HANDLE)_beginthreadex(NULL, 0, stdin_pump,
                                    (void *)(uintptr_t)sp[0], 0, NULL);
  if (!t) {
    closesocket(sp[0]);
    closesocket(sp[1]);
    return -1;
  }
  CloseHandle(t);
  return sl_sock_wrap(sp[1]);
}

int sl_console_resized(void) {
  static SHORT last_c = 0, last_r = 0;
  CONSOLE_SCREEN_BUFFER_INFO ci;
  HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
  if (h == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(h, &ci))
    return 0;
  SHORT c = (SHORT)(ci.srWindow.Right - ci.srWindow.Left + 1);
  SHORT r = (SHORT)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
  if (c != last_c || r != last_r) {
    last_c = c;
    last_r = r;
    return 1;
  }
  (void)g_resized;
  return 0;
}

/* ---- directory watching -------------------------------------------------- */

/* The server publishes the set of directories it wants watched; the pump
 * thread owns the handles that watch them. The split is not tidiness: an
 * overlapped read can only be cancelled by the thread that issued it, so
 * whoever arms a directory must also be the one to disarm it. The server
 * therefore never touches a HANDLE -- it writes `want`, bumps `generation`
 * and pokes `change`, and the pump rebuilds at a moment of its own choosing.
 *
 * Rebuilding rather than appending is what makes a reload behave: the watch
 * set is derived from the config, and a config that just changed may name
 * different files than it did a second ago. inotify and kqueue rebuild for
 * the same reason. */
#define WATCH_DIRS_MAX 8

typedef struct {
  int fd;        /* the pollable end handed to the caller */
  SOCKET notify; /* written when something changes */
  CRITICAL_SECTION lock;
  char want[WATCH_DIRS_MAX][PATH_MAX];
  int nwant;
  volatile LONG generation; /* bumped whenever `want` changes */
  HANDLE change;            /* poked with it, so the pump does not wait 200ms */
  HANDLE thread;
  volatile LONG stop;
} watcher_t;

static watcher_t *g_watchers[16];
static int g_nwatchers = 0;

static watcher_t *watcher_for(int fd) {
  for (int i = 0; i < g_nwatchers; i++)
    if (g_watchers[i] && g_watchers[i]->fd == fd) return g_watchers[i];
  return NULL;
}

#define WATCH_FILTER                                                           \
  (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |              \
   FILE_NOTIFY_CHANGE_SIZE)

/* The pump's own state: one armed directory per slot. */
typedef struct {
  HANDLE dir;
  HANDLE ev;
  OVERLAPPED ov;
  char buf[4096];
} armed_t;

static void watch_disarm(armed_t *a, int n) {
  for (int i = 0; i < n; i++) {
    if (a[i].dir != INVALID_HANDLE_VALUE) {
      /* Cancel, then wait for the cancellation to land: the OVERLAPPED and
       * its buffer live in this array, and a read still writing into them
       * after we reuse the slot would be a use-after-free with extra steps. */
      CancelIo(a[i].dir);
      DWORD got = 0;
      GetOverlappedResult(a[i].dir, &a[i].ov, &got, TRUE);
      CloseHandle(a[i].dir);
    }
    if (a[i].ev) CloseHandle(a[i].ev);
  }
}

static bool watch_arm_one(armed_t *a, const char *dir) {
  memset(a, 0, sizeof *a);
  a->dir = CreateFileA(dir, FILE_LIST_DIRECTORY,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
  if (a->dir == INVALID_HANDLE_VALUE) return false;
  a->ev = CreateEventA(NULL, TRUE, FALSE, NULL);
  a->ov.hEvent = a->ev;
  if (!a->ev || !ReadDirectoryChangesW(a->dir, a->buf, sizeof a->buf, FALSE,
                                       WATCH_FILTER, NULL, &a->ov, NULL)) {
    if (a->ev) CloseHandle(a->ev);
    CloseHandle(a->dir);
    a->dir = INVALID_HANDLE_VALUE;
    a->ev = NULL;
    return false;
  }
  return true;
}

static unsigned __stdcall watch_pump(void *arg) {
  watcher_t *w = (watcher_t *)arg;
  /* One overlapped ReadDirectoryChangesW per watched directory, all waited on
   * together with the rebuild event, each re-armed after it fires. What goes
   * down the socket is a single byte, because the reader only needs to know
   * that it should look again -- the debounce upstream turns a burst of these
   * into one reload. */
  armed_t armed[WATCH_DIRS_MAX];
  int n = 0;
  LONG armed_gen = -1;

  while (!InterlockedCompareExchange(&w->stop, 0, 0)) {
    LONG gen = InterlockedCompareExchange(&w->generation, 0, 0);
    if (gen != armed_gen) {
      /* Take a consistent snapshot of what was asked for, then arm it. The
       * lock is not held across CreateFile: the server must never wait on a
       * filesystem call to add a watch. */
      char dirs[WATCH_DIRS_MAX][PATH_MAX];
      int want;
      EnterCriticalSection(&w->lock);
      want = w->nwant;
      for (int i = 0; i < want; i++)
        snprintf(dirs[i], PATH_MAX, "%s", w->want[i]);
      LeaveCriticalSection(&w->lock);

      watch_disarm(armed, n);
      n = 0;
      for (int i = 0; i < want; i++)
        if (watch_arm_one(&armed[n], dirs[i])) n++;
      armed_gen = gen;
    }

    HANDLE waits[WATCH_DIRS_MAX + 1];
    waits[0] = w->change;
    for (int i = 0; i < n; i++) waits[i + 1] = armed[i].ev;
    DWORD r = WaitForMultipleObjects((DWORD)n + 1, waits, FALSE, 200);
    if (r == WAIT_TIMEOUT) continue;
    if (r == WAIT_OBJECT_0) { /* the set changed; rebuild on the next turn */
      ResetEvent(w->change);
      continue;
    }
    if (r < WAIT_OBJECT_0 + 1 || r >= WAIT_OBJECT_0 + 1 + (DWORD)n) break;
    int i = (int)(r - WAIT_OBJECT_0 - 1);
    DWORD got = 0;
    GetOverlappedResult(armed[i].dir, &armed[i].ov, &got, FALSE);
    ResetEvent(armed[i].ev);
    char b = 1;
    send(w->notify, &b, 1, 0);
    if (!ReadDirectoryChangesW(armed[i].dir, armed[i].buf, sizeof armed[i].buf,
                               FALSE, WATCH_FILTER, NULL, &armed[i].ov, NULL)) {
      /* The directory went away under us. Drop the slot rather than spin on a
       * handle that will never complete again; the next rebuild re-opens it
       * if the config still names it. */
      CloseHandle(armed[i].dir);
      CloseHandle(armed[i].ev);
      armed[i] = armed[--n];
    }
  }
  watch_disarm(armed, n);
  return 0;
}

int sl_watch_init(void) {
  SOCKET sp[2];
  if (raw_socketpair(sp) != 0) return -1;
  watcher_t *w = (watcher_t *)calloc(1, sizeof *w);
  if (!w) {
    closesocket(sp[0]);
    closesocket(sp[1]);
    return -1;
  }
  w->notify = sp[0];
  int fd = sl_sock_wrap(sp[1]);
  if (fd < 0) {
    closesocket(sp[0]);
    free(w);
    return -1;
  }
  u_long nb = 1;
  ioctlsocket(sp[1], FIONBIO, &nb);
  w->fd = fd;
  InitializeCriticalSection(&w->lock);
  w->change = CreateEventA(NULL, TRUE, FALSE, NULL);
  w->generation = 0;
  if (g_nwatchers < 16) g_watchers[g_nwatchers++] = w;
  /* The thread starts here rather than on the first add: it owns the armed
   * handles, so it has to be running before there is anything to arm. With an
   * empty set it waits on `change` alone, which is exactly right. */
  w->thread = (HANDLE)_beginthreadex(NULL, 0, watch_pump, w, 0, NULL);
  return fd;
}

/* Bump the generation and wake the pump. Called with the lock held. */
static void watch_republish(watcher_t *w) {
  InterlockedIncrement(&w->generation);
  SetEvent(w->change);
}

void sl_watch_clear(int fd) {
  watcher_t *w = watcher_for(fd);
  if (!w) return;
  EnterCriticalSection(&w->lock);
  w->nwant = 0;
  watch_republish(w);
  LeaveCriticalSection(&w->lock);
}

int sl_watch_add(int fd, const char *dir) {
  watcher_t *w = watcher_for(fd);
  if (!w || !dir || !*dir) return -1;
  int rc = 0;
  EnterCriticalSection(&w->lock);
  /* The same directory twice is the ordinary case -- a config and the theme
   * it includes usually live together -- and two handles on it would mean two
   * bytes per save for the debounce to swallow. Compared case-insensitively
   * because that is what the filesystem does. */
  bool have = false;
  for (int i = 0; i < w->nwant; i++)
    if (_stricmp(w->want[i], dir) == 0) have = true;
  if (!have) {
    if (w->nwant < WATCH_DIRS_MAX) {
      snprintf(w->want[w->nwant], PATH_MAX, "%s", dir);
      w->nwant++;
      watch_republish(w);
    } else {
      rc = -1;
    }
  }
  LeaveCriticalSection(&w->lock);
  return rc;
}

int sl_watch_drain(int fd) {
  char buf[256];
  int any = 0;
  for (;;) {
    long r = sl_read(fd, buf, sizeof buf);
    if (r <= 0) break;
    any = 1;
  }
  return any;
}

void sl_watch_close(int fd) {
  watcher_t *w = watcher_for(fd);
  if (w) {
    InterlockedExchange(&w->stop, 1);
    SetEvent(w->change); /* rather than waiting out the 200ms poll */
    if (w->thread) {
      WaitForSingleObject(w->thread, 2000);
      CloseHandle(w->thread);
      w->thread = NULL;
    }
    /* Joined, so the handles it owned are closed and its slot can go. */
    CloseHandle(w->change);
    DeleteCriticalSection(&w->lock);
    for (int i = 0; i < g_nwatchers; i++)
      if (g_watchers[i] == w) g_watchers[i] = NULL;
    closesocket(w->notify);
    free(w);
  }
  if (fd >= 0) sl_close(fd);
}

#endif /* _WIN32 */
