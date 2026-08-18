/* The command vocabulary, shared by the headless driver and the control
 * socket, so a script written against one works against the other. */
#ifndef SLOSH_CMD_H
#define SLOSH_CMD_H

#include "app.h"

/* Execute one command line. Returns a freshly allocated reply (possibly an
 * empty string), or NULL if the command is unknown. Sets *quit for "quit". */
char *cmd_exec(app_t *a, screen_t *s, input_parser_t *in, const char *line,
               bool *quit);

#endif /* SLOSH_CMD_H */
