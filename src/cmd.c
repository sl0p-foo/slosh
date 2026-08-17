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

/* `suspend` on a dump: which panes are written as not-yet-started. A word
 * rather than a flag because there are four honest answers and three of them
 * are wanted -- see dump_suspended() for which is the default where. */
static int suspend_policy(const char *word, int fallback) {
  if (!word) return fallback;
  if (strcmp(word, "as-is") == 0) return DUMP_SUSPEND_ASIS;
  if (strcmp(word, "none") == 0) return DUMP_SUSPEND_NONE;
  if (strcmp(word, "commands") == 0) return DUMP_SUSPEND_COMMANDS;
  if (strcmp(word, "all") == 0) return DUMP_SUSPEND_ALL;
  return -1;
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
  if (strcmp(cmd, "deadline") == 0) {
    /* When this session wants its next frame, in milliseconds, or -1 for "only
     * when something happens". A property of the frame last composed -- an
     * animated shader asks for a clock, a toast asks to expire -- so a caller
     * that has not composed one is asking about nothing. The server's poll loop
     * lives on this answer; a scripted front end has the same question. */
    return jok_int("ms", app_next_deadline_ms(a));
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
  if (strcmp(cmd, "edit-config") == 0) {
    return app_edit_config(a) ? jok_int("id", app_focused_pane_id(a))
                              : jerr("no room for another pane");
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
  /* The way in from outside the pane, for when the program that painted it will
   * not or cannot put it back. `ok` either way, with `cleared` saying whether
   * there was anything there: a script asking for a clean pane got one, and that
   * is not an error. */
  if (strcmp(cmd, "clear-shaders") == 0) {
    bool had = app_clear_pane_shaders(a, (uint32_t)jv_geti(req, "id", 0));
    return jok_int("cleared", had ? 1 : 0);
  }
  /* `tab` is an id, `0` a tab of its own -- the same "0 means the obvious thing"
   * the pane arguments use. `dir` decides beside or under, like `split`. */
  if (strcmp(cmd, "move-pane") == 0) {
    uint32_t pid = (uint32_t)jv_geti(req, "id", 0);
    uint32_t tid = (uint32_t)jv_geti(req, "tab", 0);
    bool rows = strcmp(jv_gets(req, "dir", "cols"), "rows") == 0;
    if (!tid) {
      uint32_t made = app_move_pane_to_new_tab(a, pid, jv_gets(req, "name", ""));
      if (!made) return jerr("cannot move it to a tab of its own");
      return jok_int("tab", (long long)made);
    }
    if (!app_move_pane_to_tab(a, pid, tid, rows))
      return jerr("cannot move it there");
    return jok_int("tab", (long long)tid);
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
    /* `target`, exactly as `set-purpose` already has it, because a name is a
     * label on either thing. The default stays `tab`: that is what this verb has
     * always meant, and a verb quietly changing its subject would be worse than
     * two verbs with different defaults. Naming a pane is how a stale title gets
     * overruled, so the id may be 0 for the focused one. */
    const char *target = jv_gets(req, "target", "tab");
    uint32_t id = (uint32_t)jv_geti(req, "id", 0);
    const char *name = jv_gets(req, "name", "");
    bool pane = strcmp(target, "pane") == 0;
    bool ok = pane ? app_set_pane_name(a, id, name)
                   : app_set_tab_name(a, id, name);
    return ok ? jok_int(NULL, 0) : jerr(pane ? "no such pane" : "no such tab");
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
  if (strcmp(cmd, "dump-layout") == 0) {
    dump_layout_t o = {.tab = (uint32_t)jv_geti(req, "tab", 0),
                       .base = jv_gets(req, "relative_to", NULL)};
    o.suspend = suspend_policy(jv_gets(req, "suspend", NULL), DUMP_SUSPEND_ASIS);
    if (o.suspend < 0) return jerr("suspend is as-is, none, commands or all");
    char *kdl = app_dump_layout(a, &o);
    if (o.tab && !o.tabs) {
      free(kdl);
      return jerr("no such tab");
    }
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    json_str(&j, "kdl", kdl, strlen(kdl));
    json_int(&j, "panes", (long long)o.panes);
    json_int(&j, "suspended", (long long)o.suspended);
    json_obj_close(&j);
    free(kdl);
    return j.buf;
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
  /* ---- workspaces ------------------------------------------------------- *
   *
   * `workspaces` lists what is on disk and which of it is open; the other three
   * are open, close and write one. Everything a tool needs to drive a project
   * without knowing anything about that project: open it, ask `panes` what is
   * in it, and act on the purposes the project's own layout declared. */
  if (strcmp(cmd, "workspaces") == 0) {
    project_t all[PROJECTS_MAX];
    size_t n = app_projects(all, PROJECTS_MAX);
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    /* Said out loud rather than answered with an empty list: "you have no
     * projects" and "you never said where they are" are different facts. */
    json_bool(&j, "roots", app_project_roots_set());
    json_arr_open(&j, "workspaces");
    for (size_t i = 0; i < n; i++) {
      json_obj_open(&j, NULL);
      json_str(&j, "name", all[i].name, strlen(all[i].name));
      json_str(&j, "path", all[i].path, strlen(all[i].path));
      json_str(&j, "purpose", all[i].slug, strlen(all[i].slug));
      json_str(&j, "layout", all[i].layout, strlen(all[i].layout));
      /* The file's mtime, so a tool can compare it against a pane's `since` and
       * decide the open workspace has drifted -- without this session storing a
       * byte to remember, or pretending it could re-apply a layout over running
       * processes. */
      json_int(&j, "mtime", (long long)all[i].mtime);
      json_int(&j, "tab", (long long)app_workspace_tab(a, all[i].slug));
      json_obj_close(&j);
    }
    json_arr_close(&j);
    json_obj_close(&j);
    return j.buf;
  }
  if (strcmp(cmd, "open-workspace") == 0) {
    const char *name = jv_gets(req, "name", jv_gets(req, "path", NULL));
    if (!name) return jerr("need a name or a path");
    app_workspace_open_t w;
    char err[256] = {0};
    if (!app_workspace_open(a, name, jv_getb(req, "suspended", false), &w, err,
                            sizeof err))
      return jerr(err[0] ? err : "cannot open that");
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    json_int(&j, "tab", (long long)w.tab);
    json_str(&j, "purpose", w.purpose, strlen(w.purpose));
    json_str(&j, "path", w.path, strlen(w.path));
    json_bool(&j, "created", w.created);
    json_int(&j, "tabs", (long long)w.tabs);
    /* A layout tab that declared a purpose of its own keeps it and is not a
     * member. Counted here rather than left silent, because a tab that is not
     * in the workspace it looks like it is in would be a surprise later. */
    json_int(&j, "honoured", (long long)w.honoured);
    json_obj_close(&j);
    return j.buf;
  }
  if (strcmp(cmd, "close-workspace") == 0) {
    const char *purpose = jv_gets(req, "purpose", NULL);
    char slug[64] = {0};
    if (!purpose) {
      const char *name = jv_gets(req, "name", NULL);
      if (!name) return jerr("need a name or a purpose");
      /* Resolved by the same function opening uses, so the two cannot disagree
       * about which workspace a name means. */
      project_t p;
      if (!app_workspace_find(name, &p)) return jerr("no such project");
      snprintf(slug, sizeof slug, "%s", p.slug);
      purpose = slug;
    }
    size_t n = app_workspace_close(a, purpose);
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    return jok_int("closed", (long long)n);
  }
  if (strcmp(cmd, "save-workspace") == 0) {
    int suspend = suspend_policy(jv_gets(req, "suspend", NULL),
                                 DUMP_SUSPEND_COMMANDS);
    if (suspend < 0) return jerr("suspend is as-is, none, commands or all");
    app_workspace_save_t w;
    char err[256] = {0};
    if (!app_workspace_save(a, (uint32_t)jv_geti(req, "tab", 0),
                            jv_gets(req, "path", NULL), suspend,
                            jv_getb(req, "force", false), &w, err, sizeof err))
      return jerr(err[0] ? err : "cannot save that");
    json_t j;
    json_init(&j);
    json_obj_open(&j, NULL);
    json_bool(&j, "ok", true);
    json_str(&j, "path", w.path, strlen(w.path));
    json_str(&j, "purpose", w.purpose, strlen(w.purpose));
    json_int(&j, "panes", (long long)w.panes);
    json_int(&j, "suspended", (long long)w.suspended);
    json_bool(&j, "replaced", w.replaced);
    json_obj_close(&j);
    return j.buf;
  }
  if (strcmp(cmd, "notify") == 0) {
    const char *text = jv_gets(req, "text", NULL);
    if (!text) return jerr("need text");
    app_toast(a, text);
    return jok_int(NULL, 0);
  }
  if (strcmp(cmd, "graphics") == 0) {
    app_compose(a, s);
    /* `format:"bytes"` answers with what the client's terminal is actually
     * sent, escape sequences and all. The model and the bytes are two
     * different things, and this session was one long lesson in asserting on
     * the model while the bytes were wrong. */
    if (strcmp(jv_gets(req, "format", "placements"), "bytes") == 0) {
      size_t len = 0;
      const char *raw = app_graphics(a, &len);
      json_t j;
      json_init(&j);
      json_obj_open(&j, NULL);
      json_bool(&j, "ok", true);
      json_str(&j, "bytes", raw ? raw : "", raw ? len : 0);
      json_obj_close(&j);
      return j.buf;
    }
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
    /* A complaint is not a failure: the file applied and one line of it did
     * not, so the reply is ok with a reason attached rather than an error. A
     * script that cares can look; the session says it out loud either way. */
    const char *why = app_config_complaint();
    app_toast(a, why && *why ? why : "config reloaded");
    if (why && *why) {
      json_t j;
      json_init(&j);
      json_obj_open(&j, NULL);
      json_bool(&j, "ok", true);
      json_str(&j, "warning", why, strlen(why));
      json_obj_close(&j);
      return j.buf;
    }
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
  if (strcmp(verb, "deadline") == 0) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d", app_next_deadline_ms(a));
    return strdup(buf);
  }
  if (strcmp(verb, "panes") == 0) {
    app_compose(a, s); /* layout is a function of the frame: compose first */
    return app_panes_json(a);
  }
  if (strcmp(verb, "tabs") == 0) {
    app_compose(a, s);
    return app_tabs_json(a);
  }
  if (strcmp(verb, "dump-layout") == 0) {
    app_compose(a, s); /* the layout is a function of the frame: compose first */
    return app_dump_layout(a, NULL);
  }
  /* The bare forms the harness and a person at a shell use. Same functions, so
   * a script and a test cannot drift from what the API does. */
  if (strcmp(verb, "workspaces") == 0) {
    project_t all[PROJECTS_MAX];
    size_t n = app_projects(all, PROJECTS_MAX);
    /* One row per project, each bounded by the fields it prints: name, marker
     * and path are all fixed-size in project_t, so the whole answer is too. */
    size_t cap = n * (sizeof all[0].name + sizeof all[0].path + 32) + 128;
    char *out = malloc(cap);
    size_t len = 0;
    if (!app_project_roots_set())
      len += (size_t)snprintf(out + len, cap - len,
                              "no project roots: set project_roots in your "
                              "config\n");
    for (size_t i = 0; i < n && len < cap; i++) {
      uint32_t tab = app_workspace_tab(a, all[i].slug);
      len += (size_t)snprintf(out + len, cap - len, "%-24s %-9s %s%s\n",
                              all[i].name, all[i].layout[0] ? "layout" : ".git",
                              all[i].path, tab ? "  (open)" : "");
    }
    return out;
  }
  if (strcmp(verb, "open-workspace") == 0) {
    app_workspace_open_t w;
    char err[256] = {0};
    if (!app_workspace_open(a, arg, false, &w, err, sizeof err))
      return strdup(err[0] ? err : "cannot open that");
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    char out[128];
    snprintf(out, sizeof out, "%s tab %u", w.created ? "opened" : "focused",
             w.tab);
    return strdup(out);
  }
  if (strcmp(verb, "close-workspace") == 0) {
    project_t p;
    if (!app_workspace_find(arg, &p)) return strdup("no such project");
    size_t n = app_workspace_close(a, p.slug);
    app_resize(a, s->cols, s->rows);
    s->force_full = true;
    char out[64];
    snprintf(out, sizeof out, "closed %zu tab%s", n, n == 1 ? "" : "s");
    return strdup(out);
  }
  if (strcmp(verb, "save-workspace") == 0) {
    app_workspace_save_t w;
    char err[256] = {0};
    /* The bare form takes the path a tab is not yet a workspace for, since that
     * is the one thing it cannot work out on its own. */
    if (!app_workspace_save(a, 0, *arg ? arg : NULL, DUMP_SUSPEND_COMMANDS,
                            false, &w, err, sizeof err))
      return strdup(err[0] ? err : "cannot save that");
    char out[640];
    snprintf(out, sizeof out, "%s (%zu panes, %zu suspended)", w.path, w.panes,
             w.suspended);
    return strdup(out);
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
