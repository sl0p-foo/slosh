/* The session: a layout tree of panes, focus, and the chrome around them.
 *
 * Both front ends (the interactive client and the headless driver) drive this
 * same object, so there is exactly one implementation of what a key does. */
#ifndef SL0PTTY_APP_H
#define SL0PTTY_APP_H

#include "sl0ptty.h"

typedef struct {
  uint16_t x, y, w, h;
} rect_t;

typedef enum { SPLIT_COLS, SPLIT_ROWS } split_dir_t;

typedef struct node node_t;
typedef struct app app_t;

app_t *app_new(const char *const argv[], uint16_t cols, uint16_t rows);
void app_free(app_t *a);

void app_event(app_t *a, const input_event_t *ev);
void app_resize(app_t *a, uint16_t cols, uint16_t rows);

/* Pump every pane that has data. Returns true if anything changed. */
bool app_pump_fd(app_t *a, int fd);
/* Fill `out` with every live pane fd; returns the count. */
size_t app_fds(app_t *a, int *out, size_t max);
/* Reap panes whose process exited; collapses the tree. */
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
bool app_set_tab_name(app_t *a, uint32_t id, const char *name);

/* Purposes (D8). `declared` means "from a layout/control API": it outranks an
 * in-band purpose and locks it, so a pane cannot relabel itself afterwards.
 * Returns false when a locked purpose refuses an in-band change. */
bool app_set_pane_purpose(app_t *a, uint32_t id, const char *purpose,
                          bool declared);
bool app_set_tab_purpose(app_t *a, uint32_t id, const char *purpose,
                         bool declared);

/* Address panes by id (the control API), not "the focused one". */
bool app_focus_pane(app_t *a, uint32_t id);
bool app_split_pane(app_t *a, uint32_t id, bool rows);
bool app_close_pane(app_t *a, uint32_t id);
uint32_t app_focused_pane_id(app_t *a);
uint32_t app_current_tab_id(app_t *a);

/* State as JSON, for the control API and the harness. Caller frees. */
char *app_panes_json(app_t *a);
char *app_tabs_json(app_t *a);

#endif /* SL0PTTY_APP_H */
