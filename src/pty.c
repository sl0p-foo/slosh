/* pty spawn. Hand-rolled rather than forkpty() so we need no libutil and
 * behave identically under musl.
 *
 * Windows has no fork and no pty; src/pty_win.c implements the same three
 * functions on ConPTY instead. */
#ifndef _WIN32
#define _GNU_SOURCE
#include "slosh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

int pty_spawn(pty_t *p, const char *const argv[], uint16_t cols, uint16_t rows,
              const char *cwd, uint16_t cell_w, uint16_t cell_h) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) return -1;
  if (grantpt(master) < 0 || unlockpt(master) < 0) {
    close(master);
    return -1;
  }

  char slave_name[128];
  if (ptsname_r(master, slave_name, sizeof slave_name) != 0) {
    close(master);
    return -1;
  }

  /* The slave is opened *here*, before the fork, and not by the child.
   *
   * Darwin attaches no line discipline to a pty master until its slave has
   * been opened: until then every TIOCSWINSZ on the master fails with ENOTTY
   * and the size is silently dropped. A pane is born 1x1 and told its real
   * size by the layout an instant later -- so with the child opening the
   * slave, that message raced the child's open() and usually lost, and
   * nothing ever sent it again. The pane's program spent its life believing
   * it had one row and one column, which is how `C-a e` produced an editor
   * drawing into a corner of the pane on macOS and nowhere else.
   *
   * Opening it in the parent closes the window entirely: the fork copies the
   * fd table, so the slave is open from the child's first instruction, and
   * every resize the parent sends afterwards lands. It is also what BSD
   * forkpty() does, for this reason.
   *
   * O_NOCTTY: we are not the process that wants this terminal. The child is,
   * and it says so with TIOCSCTTY after setsid(). */
  int slave = open(slave_name, O_RDWR | O_NOCTTY);
  if (slave < 0) {
    close(master);
    return -1;
  }

  /* The pixel fields matter: a program drawing images asks the tty how big a
   * cell is, and a winsize that says zero is a program that cannot size an
   * image. They are the *terminal's* pixels, so they are cols/rows times the
   * cell size the client reported.
   *
   * Set on the slave, which works on Linux and Darwin alike, and set before
   * the fork so it is ordered ahead of any resize the parent sends after it.
   * The child must not set it: by the time it ran, the size it was spawned
   * with could already be stale, and writing it back would undo the resize we
   * are here to deliver. */
  struct winsize ws = {.ws_col = cols,
                       .ws_row = rows,
                       .ws_xpixel = (unsigned short)(cols * cell_w),
                       .ws_ypixel = (unsigned short)(rows * cell_h)};
  ioctl(slave, TIOCSWINSZ, &ws);

  pid_t pid = fork();
  if (pid < 0) {
    close(slave);
    close(master);
    return -1;
  }

  if (pid == 0) {
    /* child: new session, pty becomes the controlling terminal */
    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) close(slave);
    close(master);

    if (cwd) {
      if (chdir(cwd) != 0) { /* keep going in the old cwd */
      }
    }

    /* We are opinionated about the outer terminal (D11), and we present the
     * same contract inward. xterm-ghostty is what libghostty-vt implements. */
    setenv("TERM", "xterm-ghostty", 1);
    setenv("SLOSH", "1", 1);
    unsetenv("ZELLIJ");

    signal(SIGPIPE, SIG_DFL);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }

  /* parent: the child holds the slave now, and holding a second copy would
   * keep the master readable long after the program in the pane had gone. */
  close(slave);
  int flags = fcntl(master, F_GETFL, 0);
  fcntl(master, F_SETFL, flags | O_NONBLOCK);
  p->fd = master;
  p->pid = pid;
  return 0;
}

int pty_resize(pty_t *p, uint16_t cols, uint16_t rows, uint16_t cell_w,
               uint16_t cell_h) {
  struct winsize ws = {.ws_col = cols,
                       .ws_row = rows,
                       .ws_xpixel = (unsigned short)(cols * cell_w),
                       .ws_ypixel = (unsigned short)(rows * cell_h)};
  return ioctl(p->fd, TIOCSWINSZ, &ws);
}

void pty_close(pty_t *p) {
  if (p->fd >= 0) close(p->fd);
  p->fd = -1;
  if (p->pid > 0) {
    kill(p->pid, SIGHUP);
    /* Collect it, so a session that opens and closes panes all day does not
     * accumulate zombies. Bounded and short: almost everything is gone by the
     * first pass, and a program that outlives a SIGHUP is a bigger problem
     * than the process-table entry it leaves behind. */
    for (int i = 0; i < 5; i++) {
      if (waitpid(p->pid, NULL, WNOHANG) != 0) break;
      nanosleep(&(struct timespec){0, 200000}, NULL);
    }
  }
  p->pid = -1;
}

#endif /* !_WIN32 */
