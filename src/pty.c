/* pty spawn. Hand-rolled rather than forkpty() so we need no libutil and
 * behave identically under musl. */
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

  /* The pixel fields matter: a program drawing images asks the tty how big a
   * cell is, and a winsize that says zero is a program that cannot size an
   * image. They are the *terminal's* pixels, so they are cols/rows times the
   * cell size the client reported. */
  struct winsize ws = {.ws_col = cols,
                       .ws_row = rows,
                       .ws_xpixel = (unsigned short)(cols * cell_w),
                       .ws_ypixel = (unsigned short)(rows * cell_h)};
  ioctl(master, TIOCSWINSZ, &ws);

  pid_t pid = fork();
  if (pid < 0) {
    close(master);
    return -1;
  }

  if (pid == 0) {
    /* child: new session, pty becomes the controlling terminal */
    setsid();
    int slave = open(slave_name, O_RDWR);
    if (slave < 0) _exit(127);
    ioctl(slave, TIOCSCTTY, 0);
    /* Darwin attaches no line discipline to the master until the slave is
     * opened, so the TIOCSWINSZ above returns ENOTTY there and the size is
     * dropped: the pane starts 0x0 and stays that way until something else
     * resizes it. Setting it here, on the slave we just opened, works on both. */
    ioctl(slave, TIOCSWINSZ, &ws);
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

  /* parent */
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
