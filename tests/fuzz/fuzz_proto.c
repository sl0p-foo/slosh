/* Fuzz target: the wire framing (src/proto.c).
 *
 * msg_reader_feed/next is the first thing that touches bytes off the unix
 * socket, from either side, so it must survive arbitrary garbage: lying u32
 * lengths, truncated frames, frames split at every possible boundary. The
 * input's own bytes choose the chunk sizes, so the fuzzer explores the
 * split points too.
 */
#include "proto.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  msg_reader_t r;
  msg_reader_init(&r);

  size_t i = 0;
  while (i < size) {
    size_t chunk = (size_t)(data[i] % 37) + 1; /* 1..37, data-driven splits */
    if (chunk > size - i) chunk = size - i;
    msg_reader_feed(&r, data + i, chunk);
    i += chunk;

    msg_t m;
    while (msg_reader_next(&r, &m)) {
      /* the payload must really be m.len readable bytes */
      if (m.len) {
        volatile uint8_t sink = m.data[0];
        sink = m.data[m.len - 1];
        (void)sink;
      }
    }
  }

  msg_reader_free(&r);
  return 0;
}
