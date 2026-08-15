/* OSC 5577: the pane status bar and action buttons.
 *
 * Byte-compatible with the sl0ppi fork (D1), so the pi extensions written
 * against zellij work here unmodified:
 *
 *   printf '\033]5577;1;status;building 3/7\033\\'
 *   printf '\033]5577;1;buttons;approve:Approve;cancel:Cancel\033\\'
 *   printf '\033]5577;1;clear\033\\'
 *
 * and a click comes back on the pane's stdin as
 *
 *   \033]5577;1;click;approve\033\\
 *
 * A reply is never a request. Everything the session sends back to a program
 * ends its verb in `-reply` (`hello-reply`, `shader-reply`), and no request verb
 * may, because a pane that echoes what it is sent -- `cat`, a shell with echo
 * on, a REPL waiting for a line -- would otherwise be answered into a loop. The
 * fork's `click` predates the rule and is safe for the weaker reason that
 * nothing answers a click.
 *
 * libghostty-vt's UNKNOWN_SEQUENCE effect reports APC only, not unknown OSC,
 * so we scan the pty stream ourselves. The bytes still go to lib-vt, which
 * discards an OSC it does not know — nothing is drawn, and we need no
 * buffering that could stall a pane's output.
 */
#ifndef SL0PPTY_OSC5577_H
#define SL0PPTY_OSC5577_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC5577_MAX 4096

typedef struct {
  enum { OS_GROUND, OS_ESC, OS_BODY, OS_BODY_ESC } state;
  char buf[OSC5577_MAX];
  size_t len;
  bool overflow;
} osc_scan_t;

/* verb and payload are NUL-terminated and valid for the callback only. */
typedef void (*osc5577_fn)(const char *verb, const char *payload, void *ud);

void osc_scan_reset(osc_scan_t *s);
void osc_scan_feed(osc_scan_t *s, const uint8_t *data, size_t len,
                   osc5577_fn cb, void *ud);

/* %3B %3A %25 and friends. Writes at most cap-1 bytes plus a NUL. */
size_t osc5577_unescape(const char *in, size_t in_len, char *out, size_t cap);
/* A button id is [A-Za-z0-9_-]{1,32}. Deliberately narrow: ids are echoed
 * back to the program, and a program that trusts its own ids should not have
 * to defend against what a hostile label could smuggle through. */
bool osc5577_valid_id(const char *id);

#endif /* SL0PPTY_OSC5577_H */
