/* Fuzz target: the OSC side-channel scanner (src/osc5577.c).
 *
 * This scans every byte a pane's program writes, so its input is the least
 * trusted stream in the whole session. Feed the bytes twice -- once whole,
 * once split at a data-chosen boundary so partial-sequence state is
 * exercised -- and in the callback run the payload through the unescaper at
 * several output capacities plus the id validator, exactly as pane.c would.
 *
 * OSC 7501 rides the same scanner, and its parser does the one thing 5577's
 * never had to: base64, with length limits either side of the decode. So the
 * report parser is also called directly on the raw input, which reaches body
 * shapes the scanner's own framing would never hand it.
 */
#include "osc5577.h"

#include <stdint.h>
#include <string.h>

static void on_seq(const char *verb, const char *payload, void *ud) {
  (void)ud;
  (void)osc5577_valid_id(verb);
  char out[OSC5577_MAX];
  size_t plen = strlen(payload);
  (void)osc5577_unescape(payload, plen, out, sizeof out);
  (void)osc5577_unescape(payload, plen, out, 1); /* documented minimum cap */
  (void)osc5577_unescape(payload, plen, out, 2);
  (void)osc5577_unescape(payload, plen, out, 7);
}

static void on_status(const osc7501_t *rep, void *ud) {
  (void)ud;
  /* Everything a record holds has to be a C string the rest of the session
   * can print: the parser's own contract, checked where a fuzzer can see it
   * break. */
  if (strlen(rep->id) >= sizeof rep->id) __builtin_trap();
  if (strlen(rep->app) >= sizeof rep->app) __builtin_trap();
  if (strlen(rep->msg) >= sizeof rep->msg) __builtin_trap();
  if (strlen(rep->title) >= sizeof rep->title) __builtin_trap();
  if (rep->progress < -1 || rep->progress > 100) __builtin_trap();
  (void)osc7501_state_name(rep->state);
  (void)osc7501_kind_name(rep->kind);
  (void)osc7501_is_descendant(rep->id, "");
  (void)osc7501_is_descendant(rep->id, rep->id);
}

static void on_nothing(void *ud) { (void)ud; }

static void on_query(bool bel, void *ud) {
  (void)bel;
  (void)ud;
}

static const osc_scan_cbs_t CBS = {
    .verb = on_seq,
    .status = on_status,
    .status_query = on_query,
    .prompt = on_nothing,
    .reset = on_nothing,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  osc_scan_t s;

  osc_scan_reset(&s);
  osc_scan_feed(&s, data, size, &CBS, NULL);

  /* again, split so a sequence straddles the feed boundary */
  osc_scan_reset(&s);
  size_t cut = size ? (size_t)(data[0] * (size - 1) / 255) : 0;
  osc_scan_feed(&s, data, cut, &CBS, NULL);
  osc_scan_feed(&s, data + cut, size - cut, &CBS, NULL);

  /* the report parser on its own, with no framing in the way */
  osc7501_t rep;
  if (osc7501_parse((const char *)data, size, &rep)) on_status(&rep, NULL);

  return 0;
}
