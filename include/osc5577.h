/* The OSC side-channel scanner: OSC 5577 (ours), OSC 7501 (the program status
 * protocol), OSC 133 ; A and RIS.
 *
 * One scanner, because all four want the same thing -- a look at every byte a
 * pane's program writes, before lib-vt throws away the sequences it does not
 * know -- and a pane should pay for that scan once.
 *
 * OSC 5577: the pane status bar and action buttons.
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
 *
 * OSC 7501, the program status protocol, is the other half of this file:
 *
 *   printf '\033]7501;state=working:app=cargo:msg=YnVpbGRpbmc=\033\\'
 *
 * Same problem as 5577's `status` and `busy`, solved by somebody else for
 * everybody: a program says what it is doing, in a structured way, and the
 * terminal decides how to show it. We implement it *as well as* 5577, not
 * instead of it -- 5577 is a two-way control channel (buttons answer on the
 * program's stdin, shaders are refused with a reason) and 7501 deliberately is
 * not, so neither can absorb the other. What 7501 buys is the programs we do
 * not control: an agent, cargo, brew or terraform that already speaks it needs
 * to know nothing about slosh.
 *
 * Spec: https://www.superlogical.com/rex/docs/build/program-status (rev 0.2).
 * Where it says MUST, the comments below say which line is doing it.
 *
 * Upstream libghostty-vt implements this protocol too, *newer than our pin*:
 * `GHOSTTY_TERMINAL_OPT_PROGRAM_STATUS` hands over a parsed report and leaves
 * the records to the host, which is the same split as here. On a vendor bump
 * this file's 7501 half is replaceable by that effect -- and must be, because
 * lib-vt answers the support query itself through `write_pty` and does not
 * rate-limit it. See "What upstream did" under D1 in DESIGN.md before
 * re-vendoring.
 */
#ifndef SLOSH_OSC5577_H
#define SLOSH_OSC5577_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC5577_MAX 4096

/* 7501's own cap is 4096 bytes for the whole sequence, OSC through ST. Our
 * buffer holds the body only, so the four bytes of `ESC ]` and `ESC \` come
 * off here rather than being silently allowed. */
#define OSC7501_MAX_BODY (4096 - 4)

/* How much of a `msg` or `title` we keep. The spec's limits (2048 and 192
 * bytes decoded) are what a report may *contain* and are enforced whole --
 * an over-long one is discarded, not trimmed -- but what we store is the slot
 * a pane status has always had. "Terminals MAY shorten it." */
#define OSC7501_KEEP 256

typedef enum {
  PS_NONE = 0, /* no record */
  PS_IDLE,
  PS_WORKING,
  PS_DONE,
  PS_BLOCKED,
  PS_ERROR,
  PS_CLEAR, /* not a state: removes the record and everything under it */
} ps_state_t;

typedef enum {
  PS_KIND_NONE = 0,
  PS_KIND_PERMISSION,
  PS_KIND_QUESTION,
  PS_KIND_AUTH,
} ps_kind_t;

/* One parsed report, which is also what a record holds: every key of the last
 * report that used that id, and nothing from any earlier one. */
typedef struct {
  ps_state_t state;
  ps_kind_t kind; /* blocked only */
  int progress;   /* 0..100, or -1 for unknown */
  char id[129];   /* "" is the root record */
  char app[33];   /* resolved from an ancestor when a record has none */
  char title[OSC7501_KEEP];
  char msg[OSC7501_KEEP];
} osc7501_t;

typedef struct {
  enum { OS_GROUND, OS_ESC, OS_BODY, OS_BODY_ESC } state;
  char buf[OSC5577_MAX];
  size_t len;
  bool overflow;
} osc_scan_t;

/* verb and payload are NUL-terminated and valid for the callback only. */
typedef void (*osc5577_fn)(const char *verb, const char *payload, void *ud);

/* What one pass over a pane's output can turn up. Every field may be NULL.
 *
 * `status_query` is 7501's feature detection, which a terminal answers with
 * the bytes it was sent. That is the one shape this file otherwise forbids --
 * see the note on replies above -- so the handler, not the scanner, is where
 * the loop is broken (pane.c rate-limits it).
 *
 * `prompt` (OSC 133 ; A) and `reset` (RIS, `ESC c`) are here because 7501 ties
 * record lifetime to them: a new shell prompt or a full reset is how a stale
 * `working` record dies without a heartbeat. */
typedef struct {
  void (*verb)(const char *verb, const char *payload, void *ud); /* 5577 */
  void (*status)(const osc7501_t *rep, void *ud);                /* 7501 */
  /* 7501 ; ? -- `bel` is true when the program ended its query with BEL
   * rather than ST, which is the terminator the answer has to use. */
  void (*status_query)(bool bel, void *ud);
  void (*prompt)(void *ud); /* 133 ; A */
  void (*reset)(void *ud);  /* RIS */
} osc_scan_cbs_t;

void osc_scan_reset(osc_scan_t *s);
void osc_scan_feed(osc_scan_t *s, const uint8_t *data, size_t len,
                   const osc_scan_cbs_t *cbs, void *ud);

/* Parse the pairs after `OSC 7501 ;`. False means the report is discarded
 * whole -- no state, an unknown state, a broken limit, base64 that does not
 * decode, a control character in decoded text, a malformed id -- which is the
 * spec's rule and the reason nothing here writes through a partial report.
 * A malformed *pair* is skipped and the rest still parsed. */
bool osc7501_parse(const char *pairs, size_t len, osc7501_t *out);
/* "idle", "working", "done", "blocked", "error", or "" for no record.
 * PS_CLEAR has no name: it never reaches a record. */
const char *osc7501_state_name(ps_state_t s);
/* "permission", "question", "auth", or "". */
const char *osc7501_kind_name(ps_kind_t k);
/* Whether `id` is `child` or `child/of` of `parent` ("" is the root, and the
 * ancestor of everything). Clearing and app inheritance are the only two
 * things the spec lets the hierarchy mean. */
bool osc7501_is_descendant(const char *id, const char *parent);

/* %3B %3A %25 and friends. Writes at most cap-1 bytes plus a NUL. */
size_t osc5577_unescape(const char *in, size_t in_len, char *out, size_t cap);
/* A button id is [A-Za-z0-9_-]{1,32}. Deliberately narrow: ids are echoed
 * back to the program, and a program that trusts its own ids should not have
 * to defend against what a hostile label could smuggle through. */
bool osc5577_valid_id(const char *id);

#endif /* SLOSH_OSC5577_H */
