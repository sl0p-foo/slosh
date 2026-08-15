/* Table-driven tests for the input decoder.
 *
 * Decoding is pure, so it gets pure tests: bytes in, event descriptions out.
 * Everything that needs a terminal is tested live instead (tests/live_m0.py).
 */
#include "input.h"

#include <stdio.h>
#include <string.h>

static char g_log[4096];
static size_t g_log_len;

static void collect(const input_event_t *ev, void *ud) {
  char line[256];
  input_event_describe(ev, line, sizeof line);
  g_log_len += (size_t)snprintf(g_log + g_log_len, sizeof g_log - g_log_len,
                                "%s%s", g_log_len ? " | " : "", line);
}

static int fails = 0;

static void expect(const char *name, const char *bytes, size_t len,
                   const char *want) {
  g_log[0] = 0;
  g_log_len = 0;
  input_parser_t *p = input_new();
  input_feed(p, (const uint8_t *)bytes, len, collect, NULL);
  if (input_pending(p)) input_timeout(p, collect, NULL);
  input_free(p);

  bool ok = strcmp(g_log, want) == 0;
  if (!ok) fails++;
  printf("%s %-34s %s\n", ok ? "ok  " : "FAIL", name, ok ? "" : g_log);
  if (!ok) printf("     wanted: %s\n", want);
}

#define T(name, lit, want) expect(name, lit, sizeof(lit) - 1, want)

/* Split a sequence across two feeds: a real terminal will do this to us. */
static void expect_split(const char *name, const char *bytes, size_t len,
                         size_t at, const char *want) {
  g_log[0] = 0;
  g_log_len = 0;
  input_parser_t *p = input_new();
  input_feed(p, (const uint8_t *)bytes, at, collect, NULL);
  input_feed(p, (const uint8_t *)bytes + at, len - at, collect, NULL);
  input_free(p);
  bool ok = strcmp(g_log, want) == 0;
  if (!ok) fails++;
  printf("%s %-34s %s\n", ok ? "ok  " : "FAIL", name, ok ? "" : g_log);
  if (!ok) printf("     wanted: %s\n", want);
}

int main(void) {
  /* plain text */
  T("ascii", "a", "key a mods=- press text=a");
  T("shifted ascii", "A", "key a mods=S press text=A");
  T("utf8 two byte", "\xc3\xa9", "key UNIDENTIFIED mods=- press text=\xc3\xa9");
  T("utf8 emoji", "\xf0\x9f\x92\x80",
    "key UNIDENTIFIED mods=- press text=\xf0\x9f\x92\x80");
  T("two keys", "hi", "key h mods=- press text=h | key i mods=- press text=i");

  /* C0 */
  T("ctrl-a", "\x01", "key a mods=C press");
  T("ctrl-c", "\x03", "key c mods=C press");
  T("enter", "\r", "key ENTER mods=- press text=\r");
  T("tab", "\t", "key TAB mods=- press text=\t");
  T("backspace", "\x7f", "key BACKSPACE mods=- press");
  T("ctrl-space", "\x00", "key SPACE mods=C press");

  /* escape and alt */
  T("bare esc resolves on timeout", "\x1b", "key ESCAPE mods=- press");
  T("alt-a", "\x1b" "a", "key a mods=A press text=a");
  T("alt-ctrl-a", "\x1b\x01", "key a mods=CA press");

  /* legacy CSI */
  T("up", "\x1b[A", "key ARROW_UP mods=- press");
  T("ctrl-up", "\x1b[1;5A", "key ARROW_UP mods=C press");
  T("shift-alt-left", "\x1b[1;4D", "key ARROW_LEFT mods=SA press");
  T("home", "\x1b[H", "key HOME mods=- press");
  /* Back-tab: the one chord whose shift lives in the final byte rather than
   * in a modifier param. It was falling through as an unrecognised CSI, so
   * shift+tab reached neither a binding nor the program in the pane. */
  T("shift-tab", "\x1b[Z", "key TAB mods=S press");
  T("ctrl-shift-tab", "\x1b[1;5Z", "key TAB mods=SC press");
  T("delete", "\x1b[3~", "key DELETE mods=- press");
  T("f5", "\x1b[15~", "key F5 mods=- press");
  T("ss3 f1", "\x1bOP", "key F1 mods=- press");

  /* kitty keyboard protocol */
  T("kitty a", "\x1b[97u", "key a mods=- press text=a");
  T("kitty ctrl-a", "\x1b[97;5u", "key a mods=C press");
  T("kitty release", "\x1b[97;1:3u", "key a mods=- release text=a");
  T("kitty repeat", "\x1b[97;1:2u", "key a mods=- repeat text=a");
  T("kitty esc", "\x1b[27u", "key ESCAPE mods=- press");
  T("kitty shifted", "\x1b[97:65;2u", "key a mods=S press text=A");

  /* mouse */
  T("mouse press", "\x1b[<0;10;5M", "mouse btn=1 press at=9,4 mods=-");
  T("mouse release", "\x1b[<0;10;5m", "mouse btn=1 release at=9,4 mods=-");
  T("mouse drag", "\x1b[<32;3;3M", "mouse btn=1 motion at=2,2 mods=-");
  T("wheel up", "\x1b[<64;1;1M", "mouse btn=4 press at=0,0 mods=-");
  T("ctrl-click", "\x1b[<16;2;2M", "mouse btn=1 press at=1,1 mods=C");

  /* focus */
  T("focus in", "\x1b[I", "focus in");
  T("focus out", "\x1b[O", "focus out");

  /* paste */
  T("bracketed paste", "\x1b[200~hello\x1b[201~", "paste len=5 text=hello");
  T("paste keeps control bytes", "\x1b[200~a\rb\x1b[201~",
    "paste len=3 text=a\rb");

  /* split reads: the thing that breaks naive parsers */
  expect_split("split CSI", "\x1b[1;5A", 6, 3, "key ARROW_UP mods=C press");
  expect_split("split utf8", "\xf0\x9f\x92\x80", 4, 2,
               "key UNIDENTIFIED mods=- press text=\xf0\x9f\x92\x80");
  expect_split("split paste", "\x1b[200~hi\x1b[201~", 14, 8, "paste len=2 text=hi");
  expect_split("esc then key is alt", "\x1b" "b", 2, 1, "key b mods=A press text=b");

  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all green", fails);
  return fails ? 1 : 0;
}
