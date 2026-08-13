/* One implementation of the command vocabulary, used by both the headless
 * driver and the control socket. `settle` is the exception: it needs the
 * caller's event loop, so the driver keeps it. */
#define _GNU_SOURCE
#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* \e \n \r \t \\ \0 \xHH -> bytes */
size_t cmd_unescape(const char *in, uint8_t *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in; *p && n < cap; p++) {
    if (*p != '\\') {
      out[n++] = (uint8_t)*p;
      continue;
    }
    p++;
    switch (*p) {
      case 'e': out[n++] = 0x1b; break;
      case 'n': out[n++] = '\n'; break;
      case 'r': out[n++] = '\r'; break;
      case 't': out[n++] = '\t'; break;
      case '0': out[n++] = 0; break;
      case '\\': out[n++] = '\\'; break;
      case 'x': {
        int hi = 0, lo = 0;
        sscanf(p + 1, "%1x%1x", &hi, &lo);
        out[n++] = (uint8_t)(hi * 16 + lo);
        p += 2;
        break;
      }
      case 0: return n;
      default: out[n++] = (uint8_t)*p; break;
    }
  }
  return n;
}

static void feed_event(const input_event_t *ev, void *ud) {
  app_event((app_t *)ud, ev);
}

char *cmd_exec(app_t *a, screen_t *s, input_parser_t *in, const char *line,
               bool *quit) {
  char verb[32] = {0};
  const char *arg = "";
  const char *sp = strchr(line, ' ');
  size_t vlen = sp ? (size_t)(sp - line) : strlen(line);
  if (vlen >= sizeof verb) vlen = sizeof verb - 1;
  memcpy(verb, line, vlen);
  if (sp) arg = sp + 1;

  if (strcmp(verb, "send") == 0) {
    uint8_t buf[4096];
    size_t n = cmd_unescape(arg, buf, sizeof buf);
    input_feed(in, buf, n, feed_event, a);
    if (input_pending(in)) input_timeout(in, feed_event, a);
    return strdup("");
  }
  if (strcmp(verb, "raw") == 0) {
    uint8_t buf[4096];
    size_t n = cmd_unescape(arg, buf, sizeof buf);
    app_write_focused(a, buf, n);
    return strdup("");
  }
  if (strcmp(verb, "resize") == 0) {
    int c = s->cols, r = s->rows;
    sscanf(arg, "%d %d", &c, &r);
    screen_resize(s, (uint16_t)c, (uint16_t)r);
    app_resize(a, (uint16_t)c, (uint16_t)r);
    return strdup("");
  }
  if (strcmp(verb, "snapshot") == 0) {
    app_compose(a, s);
    return strcmp(arg, "text") == 0 ? screen_dump(s) : screen_dump_json(s);
  }
  if (strcmp(verb, "panes") == 0) {
    app_compose(a, s); /* layout is a function of the frame: compose first */
    return app_panes_json(a);
  }
  if (strcmp(verb, "alive") == 0) {
    return strdup(app_should_quit(a) ? "false" : "true");
  }
  if (strcmp(verb, "quit") == 0) {
    if (quit) *quit = true;
    return strdup("");
  }
  return NULL;
}
