#define _GNU_SOURCE
#include "png.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* stb_image is already in the tree: libghostty-vt vendors it, and upstream
 * compiles that same header with exactly these defines (see
 * vendor/libghostty-vt/src/stb/stb.c) to answer exactly this question —
 * "the only use case we have right now is the Kitty Graphics protocol which
 * only supports PNG as a format besides raw RGB/RGBA buffers". So this is not
 * a new dependency; it is a file we already ship and pin, finally compiled.
 *
 * ONLY_PNG keeps the other decoders out of the binary, NO_STDIO the file
 * paths: the bytes always arrive in memory, straight off a pane's pty, so a
 * decoder that can open files is surface we have no use for. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <ghostty/vt.h>

/* The library refuses anything past this (`max_size` in
 * kitty/graphics_image.zig), so decoding past it can only ever be work done
 * to have the result thrown away. Matched rather than tightened: being
 * stricter here would silently reject images the library would have taken. */
#define MAX_DECODED (400u * 1024u * 1024u)

static bool decode(void *ud, const GhosttyAllocator *alloc, const uint8_t *data,
                   size_t data_len, GhosttySysImage *out) {
  if (!alloc || !data || !data_len || !out) return false;
  if (data_len > INT32_MAX) return false; /* stb counts in int */

  /* Read the header before decoding it. The buffer we return is allocated
   * through the library's allocator, which is wrapped in a limit — but
   * stb_image's own working allocation is plain malloc and answers to nothing,
   * so a header claiming 60000x60000 would have us try for 14GB before anyone
   * got to refuse it. This costs an IHDR parse instead. */
  int w = 0, h = 0, channels = 0;
  if (!stbi_info_from_memory(data, (int)data_len, &w, &h, &channels))
    return false;
  if (w <= 0 || h <= 0) return false;
  if ((uint64_t)w * (uint64_t)h * 4u > MAX_DECODED) return false;

  /* 4 = "give me RGBA whatever the file is", so palette, greyscale and
   * 16-bit all arrive in the one layout the protocol wants. */
  int got_w = 0, got_h = 0;
  stbi_uc *px =
      stbi_load_from_memory(data, (int)data_len, &got_w, &got_h, &channels, 4);
  if (!px) return false;
  if (got_w <= 0 || got_h <= 0) {
    stbi_image_free(px);
    return false;
  }

  /* Sized from what came back, not from what the header promised. */
  uint64_t len = (uint64_t)got_w * (uint64_t)got_h * 4u;
  if (len > MAX_DECODED) {
    stbi_image_free(px);
    return false;
  }

  /* Copied into the library's allocator because that is what it frees the
   * buffer with. Routing stb's own allocation there instead would mean
   * smuggling a per-call allocator into a global, for one memcpy that is
   * already far cheaper than the decode that produced it. */
  uint8_t *buf = ghostty_alloc(alloc, (size_t)len);
  if (!buf) {
    stbi_image_free(px);
    return false;
  }
  memcpy(buf, px, (size_t)len);
  stbi_image_free(px);

  *out = (GhosttySysImage){.width = (uint32_t)got_w,
                           .height = (uint32_t)got_h,
                           .data = buf,
                           .data_len = (size_t)len};
  return true;
}

void png_init(void) {
  ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, (const void *)(uintptr_t)decode);
}
