/* The session: a layout tree of panes, focus, and the chrome around them.
 *
 * Both front ends (the interactive client and the headless driver) drive this
 * same object, so there is exactly one implementation of what a key does. */
#ifndef SL0PPTY_APP_H
#define SL0PPTY_APP_H

#include "shader.h"
#include "sl0ppty.h"

typedef struct {
  uint16_t x, y, w, h;
} rect_t;

typedef enum { SPLIT_COLS, SPLIT_ROWS } split_dir_t;

typedef struct node node_t;
typedef struct app app_t;

app_t *app_new(const char *const argv[], uint16_t cols, uint16_t rows);
void app_free(app_t *a);

void app_event(app_t *a, const input_event_t *ev);
/* Kitty graphics for this frame: the bytes the client's terminal needs, and
 * the same thing as JSON for tests. Bytes are borrowed until the next call. */
const char *app_graphics(app_t *a, size_t *len);
void app_graphics_reset(app_t *a);
char *app_graphics_json(app_t *a);

/* Transient announcements, drawn bottom-right and expiring on their own. */
void app_toast(app_t *a, const char *text);
size_t app_toast_count(app_t *a);
/* Milliseconds until something needs repainting on its own (a toast expiring),
 * or -1 when nothing does. */
int app_next_deadline_ms(app_t *a);

/* Text the session has copied, and the copy the client has not been told
 * about yet (which the front end sends on as OSC 52). Caller frees the take. */
const char *app_clipboard(const app_t *a);
char *app_take_clipboard(app_t *a);

/* Re-read the config file. Keeps the working one if the new file is broken. */
bool app_reload_config(char *err, size_t errcap);
void app_resize(app_t *a, uint16_t cols, uint16_t rows);

/* Pump every pane that has data. Returns true if anything changed. */
bool app_pump_fd(app_t *a, int fd);
/* Fill `out` with every live pane fd; returns the count. */
size_t app_fds(app_t *a, int *out, size_t max);
/* Close panes whose process exited, collapsing the tree — only under
 * `keep_dead false`. By default a dead pane stays until it is dismissed, so
 * this does nothing and the pane offers [re-run] and [close] in its frame. */
void app_reap(app_t *a);

void app_compose(app_t *a, screen_t *s);
/* Bytes straight into the focused pane's pty (the harness's `raw`). */
void app_write_focused(app_t *a, const void *buf, size_t len);
bool app_should_quit(const app_t *a);
/* C-a d asks the client to leave while the session keeps running. */
bool app_detach_requested(const app_t *a);
void app_clear_detach(app_t *a);
size_t app_pane_count(const app_t *a);

/* Tabs. A tab is a layout tree; panes in every tab keep running. */
uint32_t app_new_tab(app_t *a, const char *name);
bool app_select_tab(app_t *a, size_t index);
bool app_select_tab_id(app_t *a, uint32_t id);
void app_cycle_tab(app_t *a, int delta);
size_t app_tab_count(const app_t *a);
bool app_close_tab(app_t *a, uint32_t id);
bool app_set_tab_name(app_t *a, uint32_t id, const char *name);
/* Put a tab at a different position in the strip, 0-based. The strip is the
 * order, so this is what dragging one does; you stay in the tab you were in. */
bool app_move_tab(app_t *a, uint32_t id, size_t index);

/* Purposes (D8). `declared` means "from a layout/control API": it outranks an
 * in-band purpose and locks it, so a pane cannot relabel itself afterwards.
 * Returns false when a locked purpose refuses an in-band change. */
bool app_set_pane_purpose(app_t *a, uint32_t id, const char *purpose,
                          bool declared);
bool app_set_tab_purpose(app_t *a, uint32_t id, const char *purpose,
                         bool declared);

/* Build tabs and panes from a KDL layout. `replace` drops what was there.
 * Purposes a layout declares are locked (D8). */
bool app_apply_layout_text(app_t *a, const char *text, bool replace, char *err,
                           size_t errcap);
bool app_apply_layout_file(app_t *a, const char *path, bool replace, char *err,
                           size_t errcap);

/* What this session is called, for the status line. The server knows it
 * because it opened the socket under that name. */
void app_set_session(app_t *a, const char *name);

/* Put a pane away into the strip along the bottom. 0 means the focused one.
 * False if it would leave the tab with nothing on screen. Restoring is done by
 * focusing it, which is what clicking its row does. */
bool app_minimize(app_t *a, uint32_t id);

/* Fill the tab with one pane, or put it back. 0 means the focused one. */
bool app_toggle_zoom(app_t *a, uint32_t id);
bool app_pane_zoomed(app_t *a, uint32_t id);

/* Address panes by id (the control API), not "the focused one". */
bool app_focus_pane(app_t *a, uint32_t id);
bool app_split_pane(app_t *a, uint32_t id, bool rows);
bool app_close_pane(app_t *a, uint32_t id);
/* Run a dead (or not-yet-started) pane's command again, in the same pane and
 * on top of the same scrollback. 0 means the focused one. */
bool app_rerun_pane(app_t *a, uint32_t id);
uint32_t app_focused_pane_id(app_t *a);
uint32_t app_current_tab_id(app_t *a);

/* Colour passes over a pane's contents (shader.h). Policy-only for now: these
 * are called from inside the app, not reachable over the control API or from
 * a program in a pane. False if the pane or the shader kind is unknown, or if
 * the pane already has SHADE_MAX of them. */
bool app_shade_add(app_t *a, uint32_t pane_id, const char *kind, color_t color,
                   uint8_t amount);
void app_shade_clear(app_t *a, uint32_t pane_id);
size_t app_shade_count(app_t *a, uint32_t pane_id);

/* State as JSON, for the control API and the harness. Caller frees. */
char *app_panes_json(app_t *a);
char *app_tabs_json(app_t *a);

#endif /* SL0PPTY_APP_H */
