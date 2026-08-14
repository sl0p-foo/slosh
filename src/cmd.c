/* One implementation of the command vocabulary, used by both the headless
 * driver and the control socket. `settle` is the exception: it needs the
 * caller's event loop, so the driver keeps it. */
#define _GNU_SOURCE
#include "cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "jsonval.h"

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

/* ---- the JSON control API (D3) ------------------------------------------
 *
 * Requests are one JSON object per line, `{"cmd": ...}`; replies are one JSON
 * object. The bare-verb form below is kept as a human/harness alias, and both
 * run the same code, so a script cannot drift from what the API does.
 */

static char *jerr(const char *msg) {
  json_t j;
  json_init(&j);
  json_obj_open(&j, NULL);
  json_bool(&j, "ok", false);
  json_str(&j, "error", msg, strlen(msg));
  json_obj_close(&j);
  return j.buf;
}

static char *jok_int(const char *key, long long val) {
  json_t j;
  json_init(&j);
  json_obj_open(&j, NULL);
  json_bool(&j, "ok", true);
  if (key) json_int(&j, key, val);
  json_obj_close(&j);
  return j.buf;
}

/* {"ok":true,"<key>":<already-serialised JSON>} */
static char *jok_raw(const char *key, char *raw) {
  size_t n = strlen(raw) + strlen(key) + 32;
  char *out = malloc(n);
  snprintf(out, n, "{\"ok\":true,\"%s\":%s}", key, raw);
  free(raw);
  return out;
}

static char *cmd_json(app_t *a, screen_t *s, input_parser_t *in,
                      const jv_t *req, bool *quit) {
  const char *cmd = jv_gets(req, "cmd", NULL);
  if (!cmd) return jerr("missing cmd");

  if (strcmp(cmd, "panes") == 0) {
    app_compose(a, s);
    return jok_raw("panes", app_panes_json(a));
  }
  if (strcmp(cmd, "tabs") == 0) {
    app_compose(a, s);
    return jok_raw("tabs", app_tabs_json(a));
  }
  if (strcmp(cmd, "snapshot") == 0) {
    app_compose(a, s);
    const char *fmt = jv_gets(req, "format", "json");
    if (strcmp(fmt, "text") == 0) {
      char *txt = screen_dump(s);
      json_t j;
      json_init(&j);
      json_obj_open(&j, NULL);
      json_bool(&j, "ok", true);
      json_str(&j, "text", txt, strlen(txt));
      json_obj_close(&j);
      free(txt);
      return j.buf;
    }
    return jok_raw("screen", screen_dump_json(s));
  }
  if (strcmp(cmd, "send") == 0) {
    const char *data = jv_gets(req, "data", "");
    input_feed(in, (const uint8_t *)data, strlen(data), feed_event, a);
    if (input_pending(in)) input_timeout(in, feed_event, a);
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "raw") == 0) {
    const char *data = jv_gets(req, "data", "");
    app_write_focused(a, data, strlen(data));
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "resize") == 0) {
    uint16_t c = (uint16_t)jv_geti(req, "cols", s->cols);
    uint16_t r = (uint16_t)jv_geti(req, "rows", s->rows);
    if (!c || !r) return jerr("bad size");
    screen_resize(s, c, r);
    app_resize(a, c, r);
    /* The cell size in pixels, which a real client reports when it attaches.
     * Here so that a script -- and the test suite -- can be a client that
     * knows its own metrics, since images are sized against these. */
    uint16_t cw = (uint16_t)jv_geti(req, "cell_w", 0);
    uint16_t ch = (uint16_t)jv_geti(req, "cell_h", 0);
    app_set_cell_px(a, cw, ch);
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "split") == 0) {
    bool rows = strcmp(jv_gets(req, "dir", "cols"), "rows") == 0;
    if (!app_split_pane(a, (uint32_t)jv_geti(req, "id", 0), rows))
      return jerr("no such pane");
    return jok_int("id", app_focused_pane_id(a));
  }
  if (strcmp(cmd, "focus") == 0) {
    if (!app_focus_pane(a, (uint32_t)jv_geti(req, "id", 0)))
      return jerr("no such pane");
    return jok_int("id", app_focused_pane_id(a));
  }
  if (strcmp(cmd, "rerun") == 0) {
    if (!app_rerun_pane(a, (uint32_t)jv_geti(req, "id", 0)))
      return jerr("cannot run it again");
    return jok_int("id", app_focused_pane_id(a));
  }
  if (strcmp(cmd, "close") == 0) {
    if (!app_close_pane(a, (uint32_t)jv_geti(req, "id", 0)))
      return jerr("no such pane");
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "new-tab") == 0) {
    uint32_t id = app_new_tab(a, jv_gets(req, "name", ""));
    if (!id) return jerr("cannot create tab");
    const char *purpose = jv_gets(req, "purpose", NULL);
    if (purpose) app_set_tab_purpose(a, id, purpose, true);
    return jok_int("id", id);
  }
  if (strcmp(cmd, "select-tab") == 0) {
    long id = jv_geti(req, "id", 0);
    bool ok = id ? app_select_tab_id(a, (uint32_t)id)
                 : app_select_tab(a, (size_t)(jv_geti(req, "index", 1) - 1));
    return ok ? jok_int("id", app_current_tab_id(a)) : jerr("no such tab");
  }
  if (strcmp(cmd, "close-tab") == 0) {
    return app_close_tab(a, (uint32_t)jv_geti(req, "id", 0))
               ? jok_int(NULL, 0)
               : jerr("no such tab");
  }
  if (strcmp(cmd, "move-tab") == 0) {
    long index = jv_geti(req, "index", 0);
    if (index < 1) return jerr("index is 1-based");
    return app_move_tab(a, (uint32_t)jv_geti(req, "id", 0), (size_t)(index - 1))
               ? jok_raw("tabs", app_tabs_json(a))
               : jerr("no such tab");
  }
  if (strcmp(cmd, "set-name") == 0) {
    return app_set_tab_name(a, (uint32_t)jv_geti(req, "id", 0),
                            jv_gets(req, "name", ""))
               ? jok_int(NULL, 0)
               : jerr("no such tab");
  }
  if (strcmp(cmd, "set-purpose") == 0) {
    const char *target = jv_gets(req, "target", "pane");
    uint32_t id = (uint32_t)jv_geti(req, "id", 0);
    const char *purpose = jv_gets(req, "purpose", "");
    /* Declared purposes come from a layout or an operator, never from a
     * pane's own output; the in-band path (M4) always passes false. */
    bool declared = jv_getb(req, "declared", true);
    bool ok = strcmp(target, "tab") == 0
                  ? app_set_tab_purpose(a, id, purpose, declared)
                  : app_set_pane_purpose(a, id, purpose, declared);
    return ok ? jok_int(NULL, 0) : jerr("refused");
  }
  if (strcmp(cmd, "apply-layout") == 0) {
    const char *path = jv_gets(req, "path", NULL);
    const char *text = jv_gets(req, "kdl", NULL);
    bool replace = jv_getb(req, "replace", false);
    char err[256] = {0};
    bool ok = text ? app_apply_layout_text(a, text, replace, err, sizeof err)
                   : path ? app_apply_layout_file(a, path, replace, err, sizeof err)
                          : false;
    if (!ok) return jerr(err[0] ? err : "need a path or kdl");
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    return jok_raw("tabs", app_tabs_json(a));
  }
  if (strcmp(cmd, "notify") == 0) {
    const char *text = jv_gets(req, "text", NULL);
    if (!text) return jerr("need text");
    app_toast(a, text);
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "graphics") == 0) {
    app_compose(a, s);
    return jok_raw("placements", app_graphics_json(a));
  }
  if (strcmp(cmd, "clipboard") == 0) {
    const char *text = app_clipboard(a);
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    json_str(&j, "text", text ? text : "", text ? strlen(text) : 0);
    json_obj_close(&j);
    return j.buf;
  }
  if (strcmp(cmd, "reload") == 0) {
    char err[256] = {0};
    if (!app_reload_config(err, sizeof err)) {
      app_toast(a, err[0] ? err : "config reload failed");
      return jerr(err[0] ? err : "reload failed");
    }
    app_resize(a, s->cols, s->rows); /* geometry may have moved */
    s->force_full = true;
    app_toast(a, "config reloaded");
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "alive") == 0) {
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    json_bool(&j, "alive", !app_should_quit(a));
    json_int(&j, "panes", (long long)app_pane_count(a));
    json_int(&j, "tabs", (long long)app_tab_count(a));
    json_obj_close(&j);
    return j.buf;
  }
  if (strcmp(cmd, "quit") == 0) {
    if (quit) *quit = true;
    return jok_int(NULL, 0);
  }
  return jerr("unknown cmd");
}

char *cmd_exec(app_t *a, screen_t *s, input_parser_t *in, const char *line,
               bool *quit) {
  while (*line == ' ') line++;
  if (*line == '{') {
    jv_t *req = jv_parse(line);
    if (!req) return jerr("malformed json");
    char *reply = cmd_json(a, s, in, req, quit);
    jv_free(req);
    return reply;
  }

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
  if (strcmp(verb, "tabs") == 0) {
    app_compose(a, s);
    return app_tabs_json(a);
  }
  if (strcmp(verb, "reload") == 0) {
    char err[256] = {0};
    bool ok = app_reload_config(err, sizeof err);
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    return strdup(ok ? "ok" : err);
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
