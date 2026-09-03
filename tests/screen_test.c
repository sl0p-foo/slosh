/* Canonical-screen projection into independently sized client views. */
#include "slosh.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ok(const char *name, bool cond) {
  if (!cond) fails++;
  printf("%s %s\n", cond ? "ok  " : "FAIL", name);
}

static char at(screen_t *s, uint16_t x, uint16_t y) {
  cell_t *c = screen_at(s, x, y);
  return c && c->len ? c->text[0] : ' ';
}

int main(void) {
  screen_t src, view;
  screen_init(&src, 6, 4);
  for (uint16_t y = 0; y < src.rows; y++)
    for (uint16_t x = 0; x < src.cols; x++) {
      char ch = (char)('A' + y * src.cols + x);
      screen_put_utf8(&src, x, y, &ch, 1, (color_t){0}, (color_t){0}, 0);
    }
  src.cursor_visible = true;
  src.cursor_x = 4;
  src.cursor_y = 2;
  uint16_t link = screen_link_id(&src, "https://example.test", 20);
  screen_set_link(&src, 3, 2, link);
  screen_at(&src, 3, 2)->attrs = ATTR_BOLD;

  screen_init(&view, 8, 5);
  screen_project(&view, &src, 0, 0);
  ok("a larger client gets the canonical top-left", at(&view, 5, 3) == 'X');
  ok("columns beyond the canonical screen are filler", at(&view, 6, 0) == ' ');
  ok("rows beyond the canonical screen are filler", at(&view, 0, 4) == ' ');
  ok("the cursor is translated into the client",
     view.cursor_visible && view.cursor_x == 4 && view.cursor_y == 2);

  screen_resize(&view, 3, 2);
  screen_project(&view, &src, 2, 1);
  ok("a smaller client receives the selected crop",
     at(&view, 0, 0) == 'I' && at(&view, 2, 1) == 'Q');
  ok("styles survive projection", (screen_at(&view, 1, 1)->attrs & ATTR_BOLD));
  ok("hyperlinks survive projection",
     view.cur_link[4] != 0 &&
         strcmp(view.links[view.cur_link[4] - 1], "https://example.test") == 0);
  ok("a cursor in the crop uses local coordinates",
     view.cursor_visible && view.cursor_x == 2 && view.cursor_y == 1);

  uint16_t vx = 0, vy = 0;
  screen_follow_cursor(&src, 3, 2, &vx, &vy);
  ok("a small viewport follows the cursor minimally", vx == 2 && vy == 1);
  src.cursor_x = 1;
  src.cursor_y = 0;
  screen_follow_cursor(&src, 3, 2, &vx, &vy);
  ok("the viewport follows the cursor back toward the origin",
     vx == 1 && vy == 0);
  src.cursor_visible = false;
  vx = 99;
  vy = 99;
  screen_follow_cursor(&src, 3, 2, &vx, &vy);
  ok("a hidden cursor retains and clamps the viewport", vx == 3 && vy == 2);

  screen_put_utf8(&src, 4, 0, "日", strlen("日"), (color_t){0}, (color_t){0},
                  0);
  screen_resize(&view, 5, 1);
  screen_project(&view, &src, 0, 0);
  ok("a wide glyph cut at the right edge is blank", at(&view, 4, 0) == ' ');
  screen_project(&view, &src, 5, 0);
  ok("a wide-glyph tail cut at the left edge is blank", at(&view, 0, 0) == ' ');

  /* The diff must not trust a wide glyph's tail column. Rule, then a wide
   * glyph over it, then rule again: overwriting the head makes the terminal
   * blank the tail cell, so the third frame has to repaint that column even
   * though its content matches what the diff last painted there. Skipping it
   * is a one-cell hole in a border, where a wide title glyph used to be. */
  screen_t d;
  screen_init(&d, 6, 1);
  for (uint16_t x = 0; x < 6; x++)
    screen_put_utf8(&d, x, 0, "-", 1, (color_t){0}, (color_t){0}, 0);
  screen_render(&d); /* frame 1: the rule */
  screen_put_utf8(&d, 2, 0, "\xe6\x97\xa5", 3, (color_t){0}, (color_t){0},
                  0); /* frame 2: 日 over columns 2-3 */
  screen_render(&d);
  screen_put_utf8(&d, 2, 0, "-", 1, (color_t){0}, (color_t){0}, 0);
  screen_put_utf8(&d, 3, 0, "-", 1, (color_t){0}, (color_t){0}, 0);
  screen_render(&d); /* frame 3: the rule again */
  /* Both rule cells must go out: an explicit CUP to column 4, or the run
   * that starts at the head's CUP simply carrying on into it. Counting the
   * plain '-' bytes in the frame answers both shapes at once. */
  size_t dashes = 0;
  for (size_t i = 0; i < d.out_len; i++)
    if (d.out[i] == '-') dashes++;
  ok("an overwritten wide glyph's tail column is repainted", dashes == 2);

  screen_free(&d);
  screen_free(&view);
  screen_free(&src);
  printf("\n%s (%d failures)\n", fails ? "FAILED" : "all green", fails);
  return fails ? 1 : 0;
}
