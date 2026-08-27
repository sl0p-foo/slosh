/* pty_spawn/resize/close on top of ConPTY.
 *
 * A ConPTY is driven through two anonymous pipes, and a pipe HANDLE cannot be
 * given to WSAPoll. So each pty gets a pair of pump threads that copy between
 * its pipes and one end of a loopback socketpair, and `pty_t.fd` is the other
 * end -- a socket, which server.c's poll set accepts alongside the session
 * socket and its clients. The event loop upstream is therefore identical to
 * the POSIX one; only what is behind the fd differs.
 *
 * `pty_t.pid` is a real Windows process id, so the existing kill/waitpid paths
 * (shimmed in compat_win.c) work on it unchanged. */
#ifdef _WIN32

#include "compat_win.h"
#include "slosh.h"

/* Per-pty state the POSIX pty_t has nowhere to put: the console handle needed
 * to resize, and the pipe ends the pumps own. Keyed by fd. */
typedef struct {
  int fd;
  HPCON hpc;
  HANDLE to_child;   /* we write -> ConPTY input */
  HANDLE from_child; /* ConPTY output -> we read */
  HANDLE proc;
  SOCKET inner; /* pump side of the socketpair */
  /* Three threads and pty_close all reach this struct, and pty_close does not
   * wait for the threads (a pump blocked on a dead pipe would hang the whole
   * session). Each holds a reference and the last one out frees. */
  volatile LONG refs;
  volatile LONG console_closed;
} conpty_t;

/* ClosePseudoConsole is what makes the ConPTY release its pipe ends, and it
 * must happen exactly once: both the exit watcher and pty_close ask for it,
 * whichever notices first. */
static void conpty_close_console(conpty_t *c) {
  if (InterlockedExchange(&c->console_closed, 1) == 0)
    ClosePseudoConsole(c->hpc);
}

static void conpty_release(conpty_t *c) {
  if (InterlockedDecrement(&c->refs) != 0) return;
  conpty_close_console(c);
  if (c->proc) CloseHandle(c->proc);
  if (c->to_child) CloseHandle(c->to_child);
  if (c->from_child) CloseHandle(c->from_child);
  if (c->inner != INVALID_SOCKET) closesocket(c->inner);
  free(c);
}

#define MAX_CONPTY 128
static conpty_t *g_ptys[MAX_CONPTY];

static conpty_t *pty_find(int fd) {
  for (int i = 0; i < MAX_CONPTY; i++)
    if (g_ptys[i] && g_ptys[i]->fd == fd) return g_ptys[i];
  return NULL;
}

static void pty_forget(conpty_t *c) {
  for (int i = 0; i < MAX_CONPTY; i++)
    if (g_ptys[i] == c) g_ptys[i] = NULL;
}

/* ConPTY output -> socket, so the pane's bytes arrive as socket readability. */
static unsigned __stdcall pump_out(void *arg) {
  conpty_t *c = (conpty_t *)arg;
  char buf[8192];
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(c->from_child, buf, sizeof buf, &got, NULL) || got == 0)
      break;
    size_t off = 0;
    while (off < got) {
      int w = send(c->inner, buf + off, (int)(got - off), 0);
      if (w <= 0) goto done;
      off += (size_t)w;
    }
  }
done:
  /* Half-close so the reader sees EOF and the pane reports its program gone,
   * which is the same signal a POSIX master gives when the slave closes. */
  shutdown(c->inner, SD_SEND);
  conpty_release(c);
  return 0;
}

/* The pane's program has exited -- now make the pane notice.
 *
 * On POSIX the pty master reports EOF once the last slave fd is closed, which
 * happens as the child dies, and pane_pump()'s read() returning 0 is the whole
 * mechanism by which a pane learns its program is gone. A ConPTY gives no such
 * signal: the output pipe belongs to the *pseudoconsole*, not to the child, so
 * it stays open after the child exits and the read never ends. A pane whose
 * shell had been told `exit` therefore sat there looking alive forever.
 *
 * So the exit is waited for explicitly, and closing the console is what
 * produces the EOF: ConPTY flushes what the child left behind, then closes its
 * end, and pump_out above sees the end of the stream exactly as it would on a
 * pty. */
static unsigned __stdcall pump_exit(void *arg) {
  conpty_t *c = (conpty_t *)arg;
  WaitForSingleObject(c->proc, INFINITE);
  conpty_close_console(c);
  conpty_release(c);
  return 0;
}

/* socket -> ConPTY input. */
static unsigned __stdcall pump_in(void *arg) {
  conpty_t *c = (conpty_t *)arg;
  char buf[8192];
  for (;;) {
    int got = recv(c->inner, buf, sizeof buf, 0);
    if (got <= 0) break;
    DWORD off = 0;
    while (off < (DWORD)got) {
      DWORD w = 0;
      if (!WriteFile(c->to_child, buf + off, (DWORD)got - off, &w, NULL) ||
          w == 0)
        goto done;
      off += w;
    }
  }
done:
  conpty_release(c);
  return 0;
}

/* Is this argv the shell being handed a command line to interpret? */
static bool is_shell_c(const char *const argv[]) {
  if (!argv[0] || !argv[1] || !argv[2]) return false;
  if (_stricmp(argv[1], "/c") != 0 && _stricmp(argv[1], "/k") != 0)
    return false;
  const char *base = argv[0];
  for (const char *p = argv[0]; *p; p++)
    if (*p == '/' || *p == '\\') base = p + 1;
  return _stricmp(base, "cmd.exe") == 0 || _stricmp(base, "cmd") == 0;
}

/* argv -> a Windows command line.
 *
 * Windows passes a *string*, not a vector, so the quoting rules belong to
 * whoever parses it at the other end -- and there are two different sets.
 * Ordinary programs are parsed by CommandLineToArgvW, where a literal quote is
 * written `\"`. cmd.exe is not: it has its own rules, and a `\"` means a
 * backslash followed by the end of a quoted section. Applying the first set to
 * the second is how `C-a e` produced a notepad complaining "Not a valid file
 * name" about a path that existed -- the editor was handed
 * `\"C:\...\config.kdl\"`, backslashes and all.
 *
 * So `cmd /c` gets the rest of the line verbatim: the caller composed a
 * complete command with its own quoting, exactly as it composes one for
 * `sh -c`, and cmd.exe is the thing that should parse it. Everything else is
 * quoted for CommandLineToArgvW as before. */
static void build_cmdline(const char *const argv[], char *out, size_t cap) {
  size_t off = 0;

  if (is_shell_c(argv)) {
    off = (size_t)snprintf(out, cap, "%s %s", argv[0], argv[1]);
    for (int i = 2; argv[i] && off < cap; i++)
      off += (size_t)snprintf(out + off, cap - off, "%s%s", i > 2 ? " " : " ",
                              argv[i]);
    if (off >= cap) out[cap - 1] = 0;
    return;
  }

  for (int i = 0; argv[i] && off + 2 < cap; i++) {
    if (i) out[off++] = ' ';
    bool q = strchr(argv[i], ' ') != NULL || argv[i][0] == 0;
    if (q && off < cap - 1) out[off++] = '"';
    for (const char *p = argv[i]; *p && off < cap - 2; p++) {
      if (*p == '"') out[off++] = '\\';
      out[off++] = *p;
    }
    if (q && off < cap - 1) out[off++] = '"';
  }
  out[off < cap ? off : cap - 1] = 0;
}

int pty_spawn(pty_t *p, const char *const argv[], uint16_t cols, uint16_t rows,
              const char *cwd, uint16_t cell_w, uint16_t cell_h) {
  (void)cell_w;
  (void)cell_h; /* ConPTY carries no pixel geometry */
  if (!p || !argv || !argv[0]) return -1;
  if (cols == 0) cols = 80;
  if (rows == 0) rows = 24;

  conpty_t *c = (conpty_t *)calloc(1, sizeof *c);
  if (!c) return -1;

  HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
  if (!CreatePipe(&in_r, &in_w, NULL, 0) ||
      !CreatePipe(&out_r, &out_w, NULL, 0)) {
    free(c);
    return -1;
  }

  COORD size = {(SHORT)cols, (SHORT)rows};
  HPCON hpc = NULL;
  HRESULT hr = CreatePseudoConsole(size, in_r, out_w, 0, &hpc);
  /* The console owns its ends now. */
  CloseHandle(in_r);
  CloseHandle(out_w);
  if (FAILED(hr)) {
    CloseHandle(in_w);
    CloseHandle(out_r);
    free(c);
    return -1;
  }

  /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE is what attaches the child to the
   * console; without it the child would inherit ours and draw over the UI. */
  STARTUPINFOEXA si = {0};
  si.StartupInfo.cb = sizeof si;
  SIZE_T attr_sz = 0;
  InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
  si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_sz);
  if (!si.lpAttributeList ||
      !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz) ||
      !UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc,
                                 sizeof hpc, NULL, NULL)) {
    free(si.lpAttributeList);
    ClosePseudoConsole(hpc);
    CloseHandle(in_w);
    CloseHandle(out_r);
    free(c);
    return -1;
  }

  char cmd[8192];
  build_cmdline(argv, cmd, sizeof cmd);

  /* Same contract as the POSIX child: we say what terminal this is. */
  SetEnvironmentVariableA("TERM", "xterm-ghostty");
  SetEnvironmentVariableA("SLOSH", "1");

  /* A child attached to a pseudoconsole is supposed to take its stdio from
   * that console -- but CreateProcess copies the *parent's* standard handles
   * into the child when STARTF_USESTDHANDLES is absent, and those win. With
   * slosh's own stdout redirected (a pipe under ssh, a file under `--script`,
   * a log under the daemon) the shell wrote its banner into slosh's output
   * instead of into the pane: the pane's title arrived, because ConPTY emits
   * that itself, while the body stayed empty.
   *
   * Clearing the three standard handles for the duration of the call leaves
   * the child nothing to inherit, so it falls back to the console it is
   * attached to -- which is the one we just made for it. */
  HANDLE saved_in = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE saved_out = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE saved_err = GetStdHandle(STD_ERROR_HANDLE);
  SetStdHandle(STD_INPUT_HANDLE, NULL);
  SetStdHandle(STD_OUTPUT_HANDLE, NULL);
  SetStdHandle(STD_ERROR_HANDLE, NULL);

  /* Same reason the config path is converted before an editor sees it: slosh
   * joins with '/' while the environment supplies '\\', and the mixture is
   * worth resolving once here rather than trusting every callee with it. */
  char cwd_native[PATH_MAX];
  const char *cwd_arg =
      (cwd && *cwd) ? sl_path_native(cwd, cwd_native, sizeof cwd_native) : NULL;

  PROCESS_INFORMATION pi = {0};
  BOOL ok =
      CreateProcessA(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                     NULL, cwd_arg, &si.StartupInfo, &pi);

  SetStdHandle(STD_INPUT_HANDLE, saved_in);
  SetStdHandle(STD_OUTPUT_HANDLE, saved_out);
  SetStdHandle(STD_ERROR_HANDLE, saved_err);

  DeleteProcThreadAttributeList(si.lpAttributeList);
  free(si.lpAttributeList);
  if (!ok) {
    ClosePseudoConsole(hpc);
    CloseHandle(in_w);
    CloseHandle(out_r);
    free(c);
    return -1;
  }
  CloseHandle(pi.hThread);

  SOCKET inner = INVALID_SOCKET;
  int outer = -1;
  if (sl_socketpair_pump(&inner, &outer) != 0) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    ClosePseudoConsole(hpc);
    CloseHandle(in_w);
    CloseHandle(out_r);
    free(c);
    return -1;
  }
  p->fd = outer; /* what the event loop polls */

  c->fd = p->fd;
  c->hpc = hpc;
  c->to_child = in_w;
  c->from_child = out_r;
  c->proc = pi.hProcess;
  c->inner = inner;

  int slot = -1;
  for (int i = 0; i < MAX_CONPTY; i++)
    if (!g_ptys[i]) {
      g_ptys[i] = c;
      slot = i;
      break;
    }
  if (slot < 0) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    ClosePseudoConsole(hpc);
    CloseHandle(in_w);
    CloseHandle(out_r);
    closesocket(inner);
    sl_close(p->fd);
    free(c);
    return -1;
  }

  /* One reference for this function's caller, one for each thread below. */
  c->refs = 1;
  HANDLE th[3];
  int nth = 0;
  InterlockedIncrement(&c->refs);
  th[nth] = (HANDLE)_beginthreadex(NULL, 0, pump_out, c, 0, NULL);
  if (th[nth])
    nth++;
  else
    InterlockedDecrement(&c->refs);
  InterlockedIncrement(&c->refs);
  th[nth] = (HANDLE)_beginthreadex(NULL, 0, pump_in, c, 0, NULL);
  if (th[nth])
    nth++;
  else
    InterlockedDecrement(&c->refs);
  InterlockedIncrement(&c->refs);
  th[nth] = (HANDLE)_beginthreadex(NULL, 0, pump_exit, c, 0, NULL);
  if (th[nth])
    nth++;
  else
    InterlockedDecrement(&c->refs);
  for (int i = 0; i < nth; i++) CloseHandle(th[i]);

  /* Non-blocking, as the POSIX build leaves its master. */
  u_long nb = 1;
  ioctlsocket(sl_sock_get(p->fd), FIONBIO, &nb);

  p->pid = (pid_t)pi.dwProcessId;
  return 0;
}

int pty_resize(pty_t *p, uint16_t cols, uint16_t rows, uint16_t cell_w,
               uint16_t cell_h) {
  (void)cell_w;
  (void)cell_h;
  if (!p) return -1;
  conpty_t *c = pty_find(p->fd);
  if (!c) return -1;
  COORD s = {(SHORT)(cols ? cols : 80), (SHORT)(rows ? rows : 24)};
  return SUCCEEDED(ResizePseudoConsole(c->hpc, s)) ? 0 : -1;
}

void pty_close(pty_t *p) {
  if (!p) return;
  conpty_t *c = pty_find(p->fd);
  /* Closing our end first is what unblocks pump_in, which is sitting in recv
   * on the other side of it. */
  if (p->fd >= 0) sl_close(p->fd);
  p->fd = -1;
  if (c) {
    pty_forget(c);
    /* Asks the child to leave, and ends pump_out's read. */
    conpty_close_console(c);
    if (c->proc && WaitForSingleObject(c->proc, 200) == WAIT_TIMEOUT)
      TerminateProcess(c->proc, 1);
    /* The threads own their own references; whichever finishes last frees. */
    conpty_release(c);
  }
  p->pid = -1;
}

#endif /* _WIN32 */
