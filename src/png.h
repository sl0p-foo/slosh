/* PNG decoding for the kitty graphics protocol.
 *
 * libghostty-vt ships no image codec: built as a library it sets its
 * `decode_png` hook to null, and a null hook means every `f=100` transmission
 * is rejected outright — no image stored, no placement, nothing for the
 * graphics layer to re-emit. A program that uploads a PNG (which is what a
 * program with a picture on disk naturally does) therefore drew nothing at
 * all, silently, while raw RGB worked. Installing this hook is the whole fix.
 *
 * PNG is the only format this can be about: the protocol has exactly three
 * (`f=24` RGB, `f=32` RGBA, `f=100` PNG) and the first two are raw pixels the
 * library already handles. Decoding only, never encoding — we hand the client
 * RGBA.
 */
#ifndef SL0PPTY_PNG_H
#define SL0PPTY_PNG_H

/* Install the process-global PNG decoder. Must be called before any terminal
 * exists, and is idempotent. */
void png_init(void);

#endif /* SL0PPTY_PNG_H */
