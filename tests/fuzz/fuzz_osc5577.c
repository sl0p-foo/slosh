/* Fuzz target: the OSC 5577 scanner (src/osc5577.c).
 *
 * This scans every byte a pane's program writes, so its input is the least
 * trusted stream in the whole session. Feed the bytes twice -- once whole,
 * once split at a data-chosen boundary so partial-sequence state is
 * exercised -- and in the callback run the payload through the unescaper at
 * several output capacities plus the id validator, exactly as pane.c would.
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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  osc_scan_t s;

  osc_scan_reset(&s);
  osc_scan_feed(&s, data, size, on_seq, NULL);

  /* again, split so a sequence straddles the feed boundary */
  osc_scan_reset(&s);
  size_t cut = size ? (size_t)(data[0] * (size - 1) / 255) : 0;
  osc_scan_feed(&s, data, cut, on_seq, NULL);
  osc_scan_feed(&s, data + cut, size - cut, on_seq, NULL);

  return 0;
}
