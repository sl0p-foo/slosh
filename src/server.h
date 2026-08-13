/* Server and client entry points. See proto.h for the wire. */
#ifndef SL0PTTY_SERVER_H
#define SL0PTTY_SERVER_H

#include <stdbool.h>
#include <stdint.h>

#include "sl0ptty.h"

/* Run the session in this process (the daemonised half). */
int server_run(const char *name, const char *const argv[], uint16_t cols,
               uint16_t rows, const char *layout);
/* Fork a server, then wait for its socket to answer. Returns a connected fd. */
int server_spawn(const char *name, const char *const argv[], uint16_t cols,
                 uint16_t rows, const char *layout);
/* Connect to an existing session, or -1. */
int server_connect(const char *name);

int client_run(int fd);
int client_control(int fd, const char *line);

#endif /* SL0PTTY_SERVER_H */
