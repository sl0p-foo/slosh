/* Server and client entry points. See proto.h for the wire. */
#ifndef SLOSH_SERVER_H
#define SLOSH_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "slosh.h"

/* Run the session in this process (the daemonised half). `watch` re-reads the
 * config when the file changes; see server.c for why that is a flag and not a
 * config setting. */
/* What panes are told about the session around them: its name, or NULL for a
 * mode that has no socket, which must clear it rather than inherit a lie. */
void session_env(const char *name);

int server_run(const char *name, const char *const argv[], uint16_t cols,
               uint16_t rows, const char *layout, bool watch);
/* Fork a server, then wait for its socket to answer. Returns a connected fd. */
int server_spawn(const char *name, const char *const argv[], uint16_t cols,
                 uint16_t rows, const char *layout, bool watch);
/* Connect to an existing session, or -1 -- with `errno` saying which kind of
 * no: ENOENT/ECONNREFUSED for "nobody is home" (a fresh session is the right
 * answer), and ETIMEDOUT for a socket that is listening and not answering,
 * which must not be replaced by a new server because something may still own
 * the session behind it. See server_connect() for the wedge this describes. */
int server_connect(const char *name);

int client_run(int fd);
int client_control(int fd, const char *line);

#endif /* SLOSH_SERVER_H */
