/* Fuzz target: the input decoder (src/input.c).
 *
 * Decodes the bytes the outer terminal sends: CSI, SS3, kitty keyboard,
 * SGR mouse, bracketed paste, focus reports -- and any terminal can send any
 * garbage. Feed in data-chosen chunks so split escape sequences are hit,
 * then resolve the pending-ESC path via input_timeout, and render every
 * event with input_event_describe so the event's fields are all read.
 */
#include "input.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void on_event(const input_event_t *ev, void *ud) {
  (void)ud;
  char buf[256];
  input_event_describe(ev, buf, sizeof buf);
  input_event_describe(ev, buf, 4); /* tiny cap must truncate, not overrun */
  if (ev->kind == EV_PASTE && ev->paste_len) {
    volatile char sink = ev->paste[0];
    sink = ev->paste[ev->paste_len - 1];
    (void)sink;
  }
  if (ev->kind == EV_KEY && ev->text_len) {
    if (ev->text_len > sizeof ev->text) abort();
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  input_parser_t *p = input_new();
  if (!p) return 0;

  size_t i = 0;
  while (i < size) {
    size_t chunk = (size_t)(data[i] % 11) + 1; /* small, split-heavy */
    if (chunk > size - i) chunk = size - i;
    input_feed(p, data + i, chunk, on_event, NULL);
    i += chunk;
  }

  if (input_pending(p)) input_timeout(p, on_event, NULL);
  input_free(p);
  return 0;
}
