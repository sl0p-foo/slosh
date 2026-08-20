/* The session: a layout tree of panes, focus, and the chrome around them.
 *
 * Both front ends (the interactive client and the headless driver) drive this
 * same object, so there is exactly one implementation of what a key does. */
#ifndef SLOSH_APP_H
#define SLOSH_APP_H

#include "kdl.h"
#include "project.h"
#include "shader.h"
#include "slosh.h"

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
/* Milliseconds until something needs repainting on its own — a toast expiring,
 * a hover guide arming, or a shader whose amount reads the clock having run
 * over the frame just composed — or -1 when nothing does. A property of that
 * frame, so it is asked after composing one. */
int app_next_deadline_ms(app_t *a);

/* Text the session has copied, and the copy the client has not been told
 * about yet (which the front end sends on as OSC 52). Caller frees the take. */
const char *app_clipboard(const app_t *a);
char *app_take_clipboard(app_t *a);

/* Re-read the config file. Keeps the working one if the new file is broken. */
bool app_reload_config(char *err, size_t errcap);
/* The files the config in force was read from: the one that was loaded and
 * everything it included, existing or not. What the watcher watches. */
size_t app_config_files(const char **out, size_t max);
/* What the config in force complained about while loading, or "". A complaint is
 * not a failure -- an include that is not there, a shader nobody has heard of --
 * so the session is running either way; this is for saying so. */
const char *app_config_complaint(void);
void app_resize(app_t *a, uint16_t cols, uint16_t rows);
/* The attached client's cell size in pixels, passed on to every pane's pty and
 * terminal. Images need it: a placement that does not say how many cells it
 * covers is sized from the image's pixels and this. Ignored if either is 0,
 * so a client that cannot find out leaves the default standing. */
void app_set_cell_px(app_t *a, uint16_t w, uint16_t h);
void app_cell_px(const app_t *a, uint16_t *w, uint16_t *h);

/* Pump every pane that has data. Returns true if anything changed. */
bool app_pump_fd(app_t *a, int fd);
/* Fill `out` with every live pane fd; returns the count. */
size_t app_fds(app_t *a, int *out, size_t max);
/* Close panes whose process exited, collapsing the tree — only under
 * `keep_dead false`. By default a dead pane stays until it is dismissed, so
 * this does nothing and the pane offers [re-run] and [close] in its frame. */
void app_reap(app_t *a);

void app_compose(app_t *a, screen_t *s);
/* Show the logo splash for splash_ms, centered over everything: the glyphs
 * fly into place first (a particle motion), then a colour effect plays over
 * them. Both are picked by the clock each time; app_splash_fx pins either by
 * index (wrapped), -1 to leave a pick to the clock. Called on attach; any
 * input ends it. */
void app_splash(app_t *a);
void app_splash_fx(app_t *a, int fx, int motion);
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
/* A pane's name, which outranks the title its program sets. 0 is the focused
 * pane; an empty name clears it and hands the label back to the program. */
bool app_set_pane_name(app_t *a, uint32_t id, const char *name);
/* Put a tab at a different position in the strip, 0-based. The strip is the
 * order, so this is what dragging one does; you stay in the tab you were in. */
bool app_move_tab(app_t *a, uint32_t id, size_t index);

/* Purposes (D8). `declared` means "from a layout/control API": it outranks an
 * in-band purpose and locks it, so a pane cannot relabel itself afterwards --
 * except by being cleared, which unlocks the slot and hands the label back,
 * because a lock over an empty string is a state nothing can escape.
 * `id` 0 is the focused pane and the current tab, as everywhere else.
 * Returns false when a locked purpose refuses an in-band change. */
bool app_set_pane_purpose(app_t *a, uint32_t id, const char *purpose,
                          bool declared);
bool app_set_tab_purpose(app_t *a, uint32_t id, const char *purpose,
                         bool declared);

/* Build tabs and panes from a KDL layout. `replace` drops what was there.
 * Purposes a layout declares are locked (D8). A relative `cwd=` in a *file*
 * resolves against that file's directory, so a project's layout can be checked
 * in beside the project; text has no directory and keeps meaning what it said. */
bool app_apply_layout_text(app_t *a, const char *text, bool replace, char *err,
                           size_t errcap);
bool app_apply_layout_file(app_t *a, const char *path, bool replace, char *err,
                           size_t errcap);
/* The same, with an explicit base -- for a layout whose text came from one place
 * and whose relative paths mean another: a shared project layout in ~/.config
 * opening a project in ~/dev. */
bool app_apply_layout_text_at(app_t *a, const char *text, bool replace,
                              const char *base, char *err, size_t errcap);

/* What a session would not honour in a layout document, one problem per entry,
 * `file:line: text` -- the same shape `--check` prints for a config, because a
 * project's layout is a file people edit by hand and `cmd=` where `command=`
 * was meant is silently a shell. Returns how many it filled in; `dropped` is
 * how many more there were, so a summary can say so. Neither allocates. */
#define LAYOUT_MSGS_MAX 32
typedef char layout_msg_t[192];
size_t layout_check(const kdl_node_t *root, const char *file,
                    layout_msg_t *msgs, size_t max, size_t *dropped);
size_t layout_check_file(const char *path, layout_msg_t *msgs, size_t max,
                         size_t *dropped);

/* ---- workspaces ---------------------------------------------------------- *
 *
 * A project is a directory (project.h). A workspace is the tab it occupies in
 * this session, and membership is that tab's `purpose` in the `project:`
 * namespace -- so there is no second answer to "what is this tab", and a dumped
 * layout restores membership for free because a dump already writes tab purposes
 * and applying one already locks them (D8).
 */

/* Whether the config names anywhere to look. False means the feature is dormant
 * and every verb below says so rather than answering with nothing. */
bool app_project_roots_set(void);
/* One project by name, or by a path under a root. False when no root holds it,
 * which is also the containment check: a path this session will not write to. */
bool app_workspace_find(const char *name, project_t *out);
/* Every project the configured roots hold, sorted by name. */
size_t app_projects(project_t *out, size_t max);
/* The tab holding a workspace, or 0. */
uint32_t app_workspace_tab(app_t *a, const char *slug);

typedef struct {
  uint32_t tab;     /* the tab to land in */
  char purpose[64]; /* the workspace's identity */
  char path[512];   /* the project on disk */
  size_t tabs;      /* tabs it adopted */
  size_t honoured;  /* tabs whose own declared purpose was left alone */
  bool created;     /* false when it was already open and was focused */
} app_workspace_open_t;

/* Open a project's layout as a workspace, or focus the one already open --
 * idempotent, so the same request twice is one workspace and a caller never has
 * to ask first. `name` is a project name or a path under a root. `suspended`
 * starts every pane asleep whatever the layout said, which is the "open ten
 * projects, run zero processes" case. */
bool app_workspace_open(app_t *a, const char *name, bool suspended,
                        app_workspace_open_t *out, char *err, size_t errcap);

/* Close every tab carrying this workspace's purpose; returns how many. */
size_t app_workspace_close(app_t *a, const char *slug);

typedef struct {
  char path[512];   /* the file written */
  char purpose[64]; /* the workspace the tab now carries */
  size_t panes, suspended;
  bool replaced; /* there was a layout there before */
} app_workspace_save_t;

/* Write one tab out as a project's layout: the same dump `dump-layout` answers
 * with, relative to the project, into `slosh.layout` beside it. `tab` 0 is
 * the current one; `path` names the project for a tab that is not yet a
 * workspace (and saving adopts it, which is the whole of onboarding one).
 * Refused without `force` when the project already has a layout: that file is
 * checked in. */
bool app_workspace_save(app_t *a, uint32_t tab, const char *path, int suspend,
                        bool force, app_workspace_save_t *out, char *err,
                        size_t errcap);

/* What this session is called, for the status line. The server knows it
 * because it opened the socket under that name. */
void app_set_session(app_t *a, const char *name);

/* Put a pane away into the strip along the bottom. 0 means the focused one.
 * False if it would leave the tab with nothing on screen. Restoring is done by
 * focusing it, which is what clicking its row does. */
bool app_minimize(app_t *a, uint32_t id);

/* Move a pane into another tab, beside whatever that tab has focused. 0 means the
 * focused pane; `rows` puts it under rather than beside. The destination is a tab
 * *id*, not an index: emptying the source tab removes it and shifts every index
 * after it, which an id survives.
 *
 * The pane keeps running -- nothing is re-spawned, which is the point of moving one
 * rather than opening another and closing this. False when there is nothing to do:
 * no such pane, no such tab, or it is already there. Which tab you are looking at
 * does not change.
 *
 * A move drops what the old tab thought about the pane: a zoom naming it, and its
 * minimised flag. Carried across, the first would zoom a pane that has left and the
 * second would file the arrival in a strip nobody asked for. */
bool app_move_pane_to_tab(app_t *a, uint32_t pane_id, uint32_t tab_id,
                          bool rows);

/* The same, into a tab of its own; returns that tab's id, or 0 -- including for a
 * pane that is already alone in its tab, which has nowhere to go. */
uint32_t app_move_pane_to_new_tab(app_t *a, uint32_t pane_id, const char *name);

/* Fill the tab with one pane, or put it back. 0 means the focused one. */
bool app_toggle_zoom(app_t *a, uint32_t id);
bool app_pane_zoomed(app_t *a, uint32_t id);

/* Lift a pane out of the layout and draw it on top, or put it back in the
 * seat it kept. 0 means the focused one. False when it would leave the tab
 * with no tiled pane to float over. */
bool app_toggle_float(app_t *a, uint32_t id);
bool app_pane_floating(app_t *a, uint32_t id);

/* Float a pane at a wanted rect: floats it first when it is tiled, re-places
 * it when it already floats. Negative x/y and zero w/h mean "keep". */
bool app_float_place(app_t *a, uint32_t id, long x, long y, long w, long h);

/* A new floating shell over the current tab, centred: the throwaway
 * terminal. It holds a real seat in the tree beside the focused pane, so
 * un-floating lands it there. Returns its id, or 0. */
uint32_t app_new_float(app_t *a);

/* Give every visible pane in the current tab an even share of the rows and
 * columns it competes for: each split's children weighted by how many visible
 * panes are behind them, so "even" means the same thing at every depth. False
 * when the tab is a single pane and there is nothing to divide. */
bool app_equalize_splits(app_t *a);

/* Address panes by id (the control API), not "the focused one". */
bool app_focus_pane(app_t *a, uint32_t id);
bool app_split_pane(app_t *a, uint32_t id, bool rows);
bool app_close_pane(app_t *a, uint32_t id);
/* Run a dead (or not-yet-started) pane's command again, in the same pane and
 * on top of the same scrollback. 0 means the focused one. */
bool app_rerun_pane(app_t *a, uint32_t id);

/* Turn the current tab's layout a quarter turn clockwise: every split changes
 * axis, and the children of a row split reverse, because the pane on top of a
 * stack is the pane on the right once you turn it. Four turns are the identity.
 * False when the tab is a single pane, which looks the same from every angle. */
bool app_rotate_layout(app_t *a);
/* Open the config file in $EDITOR, in a pane of its own, so the edit/save/
 * watch loop happens without leaving the session. The pane is ephemeral. */
bool app_edit_config(app_t *a);
uint32_t app_focused_pane_id(app_t *a);
uint32_t app_current_tab_id(app_t *a);

/* Colour passes over one pane (shader.h), set by the program running in it over
 * OSC 5577. `text` is a *document* in the config's own syntax -- one entry, several
 * separated by `;`, or a whole `shaders { }` block -- and each pass goes where its
 * own `where=` says, so both chains are set from one call. `default_chrome` is what
 * a pass that does not say means, which is how a prompt aimed at the frame works
 * without every line repeating itself. Empty text clears both.
 *
 * Counts come back so a caller can say what happened: "ok" alone cannot tell a
 * block that filled both chains from one whose entries were all dropped. Refused
 * with a reason in `err`: `in_band_shaders` off, no such pane, or text that does
 * not read. */
bool app_set_pane_shaders(app_t *a, uint32_t pane_id, bool default_chrome,
                          const char *text, size_t *nchrome, size_t *ncontent,
                          char *err, size_t errcap);

/* A preset file -- `contrib/chrome/sine-comet.kdl` and its neighbours -- applied
 * to one pane, both chains at once, routed by each entry's own `where=`. The
 * session reads the file so that nothing else has to know this format; a relative
 * path resolves against the session's working directory. Counts come back so a
 * caller can say what happened. Gated on `in_band_shaders` like setting a chain by
 * hand: it is the same act with a shorter spelling. */
bool app_load_pane_shaders(app_t *a, uint32_t pane_id, const char *path,
                           size_t *nchrome, size_t *ncontent, char *err,
                           size_t errcap);

/* Undo both of one pane's own chains. 0 means the focused pane. False when there
 * was nothing to undo, so a caller can say so rather than flash a frame nobody
 * changed.
 *
 * Deliberately not gated on `in_band_shaders`: a chain can outlive the consent
 * that allowed it, and the way out must not be the thing the setting controls.
 * Leaves the config's chains and the session's policy passes alone -- neither is
 * this pane's doing. */
bool app_clear_pane_shaders(app_t *a, uint32_t pane_id);

/* What to write when a session is written back out. `tab` 0 is every tab;
 * `base` relativises every `cwd=` under it, which is what makes a project's
 * layout the same file on another machine; `suspend` decides which panes are
 * written as not-yet-started. `panes` and `suspended` come back filled in, so
 * a caller can say what it wrote. NULL means all of it, verbatim, absolute. */
typedef enum {
  DUMP_SUSPEND_ASIS = 0, /* what is suspended now */
  DUMP_SUSPEND_NONE,
  DUMP_SUSPEND_COMMANDS, /* every pane that was given a command */
  DUMP_SUSPEND_ALL
} dump_suspend_t;

typedef struct {
  uint32_t tab;
  const char *base;
  int suspend;
  /* This dump *is* a project's own layout file, so leave out what the project's
   * location already says: a tab's `project:` purpose is derived from the path,
   * and a checked-in file carrying one would hand another checkout of the same
   * repo the original's identity. Every other purpose is somebody's label and is
   * written as usual. */
  bool for_project;
  /* out. `tabs` is how a caller tells "that tab has no panes" from "there is no
   * such tab", which are the same empty document. */
  size_t tabs, panes, suspended;
} dump_layout_t;

/* The session written back out as a layout file: tabs, splits, proportions,
 * directories and commands. The inverse of app_apply_layout_text(), and what
 * `dump-layout` and `save-workspace` both answer with -- one of them writes it
 * to a file, which is the only difference between them.
 *
 * What a dump can honestly restore is the shape. Not the state inside a
 * program: a shell's history, a running editor. A pane running the session's
 * default shell is dumped as a pane with no command, so restoring it gives a
 * fresh one, and a pane whose program has exited is dumped as what it ran. It
 * does not pretend otherwise. */
char *app_dump_layout(app_t *a, dump_layout_t *o);

/* State as JSON, for the control API and the harness. Caller frees. */
char *app_panes_json(app_t *a);
char *app_tabs_json(app_t *a);

#endif /* SLOSH_APP_H */
