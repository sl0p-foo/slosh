/* Windows compatibility shim for slosh.
 *
 * The port's one idea: *everything the event loop waits on is a socket*.
 *
 * On POSIX a pty master, an inotify fd and a unix socket are all fds, so one
 * poll() waits on all of them. Windows has no such union -- a ConPTY hands
 * back pipe HANDLEs, a directory watch is a completion callback, and WSAPoll
 * waits on SOCKETs and nothing else. Rather than rewrite server.c's loop
 * around WaitForMultipleObjects (which caps at 64 and cannot express POLLOUT),
 * each non-socket source gets a pump thread that copies it into one end of a
 * loopback socketpair. The loop then polls sockets exclusively and its
 * structure is unchanged from the POSIX build.
 *
 * Socket "fds" are tagged integers (SL_SOCK_BASE + slot) rather than raw
 * SOCKET values, so read/write/close can tell a socket from a CRT fd with
 * certainty instead of guessing from the numeric range.
 *
 * System headers are included *before* the POSIX names are macro-defined, so
 * that e.g. unistd.h's own `read` declaration is seen as itself and not
 * rewritten into ours. */
#ifndef SLOSH_COMPAT_WIN_H
#define SLOSH_COMPAT_WIN_H

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10+: AF_UNIX, ConPTY, WSAPoll */
#endif

/* winsock2 before windows.h, or windows.h drags in the winsock 1.1 headers
 * and every socket symbol collides. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>

/* Everything that declares a name we are about to macro-define is pulled in
 * here first, so those declarations are read as themselves rather than being
 * rewritten (this header is force-included ahead of the source's own
 * includes, so there is no second chance). */
#include <direct.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* windows.h is generous with short macro names, and several are words slosh
 * uses as enum constants. Undefining them here (rather than renaming the enums)
 * keeps the shim's cost inside the shim. RegisterHotKey's MOD_* are the ones
 * that actually collide; the rest are common enough to be worth pre-empting. */
#undef MOD_SHIFT
#undef MOD_CONTROL
#undef MOD_ALT
#undef MOD_WIN
#undef DELETE
#undef ERROR
#undef OPTIONAL
#undef IN
#undef OUT
#undef near
#undef far
#undef small

#ifdef __cplusplus
extern "C" {
#endif

/* ---- small gaps in the C library ---------------------------------------- */

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (0) /* no symlinks worth chasing here */
#endif

typedef unsigned long nfds_t;

char *sl_strndup(const char *s, size_t n);
char *sl_realpath(const char *path, char *resolved);
long sl_readlink(const char *path, char *buf, size_t cap);
int sl_setenv(const char *k, const char *v, int overwrite);
int sl_unsetenv(const char *k);
int sl_nanosleep_ms(long ms);

/* ---- fcntl / open flags -------------------------------------------------- */

#ifndef O_NONBLOCK
#define O_NONBLOCK 0x40000000
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_EVTONLY
#define O_EVTONLY 0
#endif
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#endif
int sl_fcntl(int fd, int cmd, ...);

/* ---- signals we only ever name ------------------------------------------- */

#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGKILL
#define SIGKILL 9
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGWINCH
#define SIGWINCH 28
#endif
#ifndef SIGCHLD
#define SIGCHLD 17
#endif
#ifndef SIG_SETMASK
#define SIG_SETMASK 2
#endif

#ifndef WNOHANG
#define WNOHANG 1
#endif
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)

int sl_kill(pid_t pid, int sig);
pid_t sl_waitpid(pid_t pid, int *status, int options);

/* ---- terminal size ------------------------------------------------------- */

struct winsize {
  unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};
#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY 0x540E
#endif
int sl_ioctl(int fd, unsigned long req, ...);

/* ---- the console, in place of termios ------------------------------------ */

/* Raw mode is ENABLE_VIRTUAL_TERMINAL_INPUT on the input handle plus
 * ENABLE_VIRTUAL_TERMINAL_PROCESSING on the output, which is how a modern
 * conhost speaks the same escape sequences the POSIX client already writes. */
bool sl_console_raw(void);
void sl_console_restore(void);
/* stdin as a pollable socket: a thread reads the console and forwards. */
int sl_console_stdin_socket(void);
/* Signals the console delivers as events rather than as signals. */
int sl_console_resized(void); /* 1 once per observed size change */

/* ---- sockets ------------------------------------------------------------- */

/* Tagged so read/write/close can dispatch without guessing. */
#define SL_SOCK_BASE 0x40000000

int sl_sock_wrap(SOCKET s);
SOCKET sl_sock_get(int fd);
static inline int sl_is_sock(int fd) { return fd >= SL_SOCK_BASE; }

/* The BSD socket calls, tagged. Without these, socket()/accept() would hand
 * back a raw SOCKET that read/write/poll would mistake for a CRT fd -- which
 * is exactly the kind of ambiguity the tagging exists to remove. */
int sl_socket(int domain, int type, int proto);
int sl_bind(int fd, const struct sockaddr *a, int len);
int sl_listen(int fd, int backlog);
int sl_accept(int fd, struct sockaddr *a, int *len);
int sl_connect(int fd, const struct sockaddr *a, int len);
long sl_send(int fd, const void *b, size_t n, int flags);
long sl_recv(int fd, void *b, size_t n, int flags);
/* An AF_UNIX socket file is a reparse point, so removing a stale one is
 * DeleteFile rather than anything the CRT offers. */
int sl_unlink(const char *path);

int sl_socketpair(int fds[2]);
/* Half-tagged pair, for the pump threads: `inner` stays a raw SOCKET owned by
 * a pump, `outer_fd` is the tagged fd the event loop polls. */
int sl_socketpair_pump(SOCKET *inner, int *outer_fd);
long sl_read(int fd, void *buf, size_t n);
long sl_write(int fd, const void *buf, size_t n);
int sl_close(int fd);

struct sl_pollfd {
  int fd;
  short events;
  short revents;
};
int sl_poll(struct sl_pollfd *fds, nfds_t n, int timeout_ms);

/* ---- dynamic loading ----------------------------------------------------- */

#ifndef RTLD_NOW
#define RTLD_NOW 0
#define RTLD_LOCAL 0
#define RTLD_LAZY 0
#define RTLD_GLOBAL 0
#endif
void *sl_dlopen(const char *path, int flags);
void *sl_dlsym(void *lib, const char *sym);
int sl_dlclose(void *lib);
const char *sl_dlerror(void);

/* ---- glob ---------------------------------------------------------------- */

typedef struct {
  size_t gl_pathc;
  char **gl_pathv;
  size_t gl_offs;
} glob_t;
#ifndef GLOB_NOSORT
#define GLOB_NOSORT 0
#define GLOB_MARK 0
#define GLOB_NOMATCH 3
#define GLOB_TILDE 0
#endif
int sl_glob(const char *pat, int flags, int (*errfn)(const char *, int),
            glob_t *g);
void sl_globfree(glob_t *g);

/* ---- directory watching (in place of inotify / kqueue) ------------------- */

/* One watcher: a socket you can poll, fed by a ReadDirectoryChangesW thread.
 * The payload is deliberately not inotify's, because server.c's Windows branch
 * reads it as "something under a watched directory changed" and re-stats the
 * files it cares about -- the same conclusion its kqueue branch reaches. */
int sl_watch_init(void); /* -> pollable fd, or -1 */
/* The watch set is rebuilt, not appended to: clear, then add what the config
 * names now. A reload may name different files than the last one did, and the
 * inotify and kqueue branches rebuild for that reason too. */
void sl_watch_clear(int fd);
int sl_watch_add(int fd, const char *dir);
void sl_watch_close(int fd);
int sl_watch_drain(int fd); /* consume readiness; 1 if anything changed */

/* ---- process helpers ----------------------------------------------------- */

/* argv of a live process, for the pane title. */
bool sl_proc_cmdline(pid_t pid, char *out, size_t cap);
/* A pty has no foreground process group here, so "what is this pane running"
 * is answered with the most recently started descendant of the pane's shell --
 * which is the process tcgetpgrp() would have named on POSIX. */
pid_t sl_proc_foreground(pid_t root);
/* the running executable's own path, in place of /proc/self/exe. */
bool sl_self_exe(char *out, size_t cap);
/* spawn ourselves detached, in place of fork()+setsid() for the daemon. */
int sl_spawn_detached(const char *const argv[], const char *logpath);
/* `which`: is this program on PATH? Used to pick an editor that will actually
 * draw inside a pane. */
bool sl_which(const char *exe);

/* A path as a Windows *program* expects to see it: absolute, with backslashes.
 *
 * slosh builds paths with '/' throughout, because that is what it splits and
 * compares on, and mixing the two is unavoidable the moment a '/' literal is
 * joined to an environment variable that came back with '\\' -- $HOME is
 * `C:\Users\you`, so the config lands at `C:\Users\you/.config/slosh/...`.
 * The CRT accepts that happily. GUI file dialogs do not: notepad answers a
 * mixed-separator path with "Not a valid file name". Anything handed to
 * another program goes through here first. */
const char *sl_path_native(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

/* ---- the POSIX names, defined last --------------------------------------- */

#define strndup sl_strndup
#define realpath sl_realpath
#define readlink sl_readlink
#define setenv sl_setenv
#define unsetenv sl_unsetenv

#define mkdir(p, m) _mkdir(p)
#define fcntl sl_fcntl
#define ioctl sl_ioctl
#define kill sl_kill
#define waitpid sl_waitpid

#define read sl_read
#define write sl_write
#define close sl_close
#define poll sl_poll
#define pollfd sl_pollfd
#define socketpair(d, t, p, sv) sl_socketpair(sv)
#define unlink sl_unlink
/* Permissions on the session socket are the directory's job here: LOCALAPPDATA
 * is already per-user, and Windows has no mode bits to set on it. */
#define chmod(p, m) (0)

/* The shim's own translation units call the real socket API, so they are
 * compiled with SL_COMPAT_IMPL and see through these. */
#ifndef SL_COMPAT_IMPL
#define socket sl_socket
#define bind sl_bind
#define listen sl_listen
#define accept sl_accept
#define connect sl_connect
#define send sl_send
#define recv sl_recv
#endif

#define dlopen sl_dlopen
#define dlsym sl_dlsym
#define dlclose sl_dlclose
#define dlerror sl_dlerror

#define glob sl_glob
#define globfree sl_globfree

/* ssize_t is what the callers assign our long returns to. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long long ssize_t;
#endif

#endif /* _WIN32 */
#endif /* SLOSH_COMPAT_WIN_H */
