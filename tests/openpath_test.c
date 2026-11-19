/* Clicking a path open, the pure half: where a path starts and stops in text,
 * what the numbers after it mean, which directory a relative one meant, and how
 * each editor is told the line. */
#define _GNU_SOURCE
#include "openpath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails = 0;

static void ok(const char *name, bool cond, const char *detail) {
  if (!cond) fails++;
  printf("%s %-58s %s\n", cond ? "ok  " : "FAIL", name, cond ? "" : detail);
}

static void eq(const char *name, const char *got, const char *want) {
  bool same = got && want && strcmp(got, want) == 0;
  char detail[1024];
  snprintf(detail, sizeof detail, "got [%s], wanted [%s]", got ? got : "(null)",
           want);
  ok(name, same, detail);
}

/* Scan `text` with the pointer on the first byte of `on`. */
static bool scan_on(const char *text, const char *on, openpath_ref_t *r) {
  const char *at = strstr(text, on);
  if (!at) return false;
  return openpath_scan(text, strlen(text), (size_t)(at - text), r);
}

static void scans(const char *name, const char *text, const char *on,
                  const char *path, long line, long col) {
  openpath_ref_t r;
  bool got = scan_on(text, on, &r);
  char detail[1024];
  snprintf(detail, sizeof detail, "got %s [%s] %ld:%ld", got ? "yes" : "no",
           got ? r.path : "", got ? r.line : 0, got ? r.col : 0);
  ok(name, got && strcmp(r.path, path) == 0 && r.line == line && r.col == col,
     detail);
}

static void refuses(const char *name, const char *text, const char *on) {
  openpath_ref_t r;
  bool got = scan_on(text, on, &r);
  ok(name, !got, got ? r.path : "");
}

static void touch(const char *p) {
  FILE *f = fopen(p, "w");
  if (f) fclose(f);
}

int main(void) {
  /* ---- scanning --------------------------------------------------------- */
  scans("a bare relative path", "edit src/app.c now", "app", "src/app.c", 0, 0);
  scans("clicked on its first byte", "edit src/app.c now", "src", "src/app.c",
        0, 0);
  scans("gcc's file:line:col: and the trailing colon",
        "src/app.c:1450:12: error: nope", "app", "src/app.c", 1450, 12);
  scans("clicked on the number itself", "src/app.c:1450:12: error", "1450",
        "src/app.c", 1450, 12);
  scans("grep -n's file:line:", "src/app.c:88:  int x;", "src", "src/app.c", 88,
        0);
  scans("rustc's arrow", "  --> src/main.rs:10:5", "main", "src/main.rs", 10,
        5);
  scans("sentence punctuation is not the name", "see ./notes.md.", "notes",
        "./notes.md", 0, 0);
  scans("quoted", "open \"docs/x.md\" now", "x.md", "docs/x.md", 0, 0);
  scans("in brackets", "(see include/app.h)", "include", "include/app.h", 0, 0);
  scans("MSVC's (line,col)", "C:\\x\\a.c(12,5): error", "a.c", "C:\\x\\a.c", 12,
        5);
  scans("MSVC's (line)", "a.c(12): warning", "a.c", "a.c", 12, 0);
  scans("python's traceback", "  File \"/srv/app.py\", line 42, in run",
        "app.py", "/srv/app.py", 42, 0);
  scans("home-relative", "cat ~/.bashrc", "bashrc", "~/.bashrc", 0, 0);
  scans("a git diff header", "+++ b/src/pane.c", "pane", "b/src/pane.c", 0, 0);
  scans("a --flag=value keeps the value", "--output=build/x.o", "build",
        "build/x.o", 0, 0);
  scans("a non-ascii name", "vim notes/café.md", "caf", "notes/café.md", 0, 0);
  scans("dots alone are a directory", "cd ..", "..", "..", 0, 0);
  scans("a long hex run is not a line number", "x.c:deadbeef", "x.c",
        "x.c:deadbeef", 0, 0);
  scans("a file:// URL in the text is a path", "file:///etc/hosts", "file",
        "/etc/hosts", 0, 0);
  {
    const char *t = "see (src/app.c:12:5), ok";
    openpath_ref_t r;
    bool got = scan_on(t, "app", &r);
    char span[64] = "";
    if (got && r.end > r.start && r.end - r.start < sizeof span)
      memcpy(span, t + r.start, r.end - r.start);
    eq("the span keeps the numbers and drops the punctuation", span,
       "src/app.c:12:5");
  }
  refuses("on a space", "a  b", " ");
  refuses("an https URL is somebody else's", "https://example.com/a.c",
          "example");
  refuses("on a bracket", "(x)", "(");

  /* ---- file:// links ---------------------------------------------------- */
  openpath_ref_t r;
  ok("file:///abs", openpath_from_uri("file:///etc/hosts", 17, "box", &r), "");
  eq("  its path", r.path, "/etc/hosts");
  ok("our own hostname is us",
     openpath_from_uri("file://box/etc/hosts", 20, "box", &r), "");
  ok("localhost is us",
     openpath_from_uri("file://localhost/etc/hosts", 26, "box", &r), "");
  ok("another host is not",
     !openpath_from_uri("file://elsewhere/etc/hosts", 26, "box", &r), "");
  const char *pct = "file:///tmp/a%20b.txt#L7";
  ok("percent-decoded, with a fragment line",
     openpath_from_uri(pct, strlen(pct), "box", &r), "");
  eq("  its path", r.path, "/tmp/a b.txt");
  ok("  and its line", r.line == 7, "");
  ok("not a file URI", !openpath_from_uri("https://x/y", 11, "box", &r), "");

  /* ---- resolving -------------------------------------------------------- */
  char tmpl[] = "/tmp/openpath_test.XXXXXX";
  char *root = mkdtemp(tmpl);
  char sub[512], p[1024], got[4096];
  snprintf(sub, sizeof sub, "%s/sub", root);
  mkdir(sub, 0755);
  snprintf(p, sizeof p, "%s/.git", root);
  mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/top.c", root);
  touch(p);
  snprintf(p, sizeof p, "%s/here.c", sub);
  touch(p);

  const char *dirs[] = {sub, root};
  ok("relative to the first dir",
     openpath_resolve("here.c", dirs, 2, got, sizeof got), "");
  snprintf(p, sizeof p, "%s/here.c", sub);
  eq("  joined", got, p);
  ok("falls through to the next dir",
     openpath_resolve("top.c", dirs, 2, got, sizeof got), "");
  ok("./ is dropped", openpath_resolve("./here.c", dirs, 2, got, sizeof got),
     "");
  eq("  without it", got, p);
  ok("a/ from a diff", openpath_resolve("a/top.c", dirs, 2, got, sizeof got),
     "");
  ok("absolute", openpath_resolve(p, dirs, 0, got, sizeof got), "");
  ok("a directory counts",
     openpath_resolve("sub", dirs + 1, 1, got, sizeof got), "");
  ok("a word that is no file is no path",
     !openpath_resolve("nope.c", dirs, 2, got, sizeof got), "");

  ok("the git root from below it", openpath_git_root(sub, got, sizeof got), "");
  eq("  is the root", got, root);
  ok("no git root above /", !openpath_git_root("/", got, sizeof got), "");

  snprintf(p, sizeof p, "rm -rf '%s'", root);
  if (system(p) != 0) printf("(could not clean %s)\n", root);

  /* ---- commands --------------------------------------------------------- */
  char cmd[8192];
  openpath_command(NULL, "nvim", "/a/b.c", 12, 5, cmd, sizeof cmd);
  eq("nvim with line and col", cmd, "nvim '+call cursor(12,5)' '/a/b.c'");
  openpath_command(NULL, "/usr/bin/vim -p", "/a/b.c", 12, 0, cmd, sizeof cmd);
  eq("vim by full path, with args", cmd, "/usr/bin/vim -p +12 '/a/b.c'");
  openpath_command(NULL, "nano", "/a/b.c", 12, 5, cmd, sizeof cmd);
  eq("nano", cmd, "nano +12,5 '/a/b.c'");
  openpath_command(NULL, "emacsclient -t", "/a/b.c", 3, 4, cmd, sizeof cmd);
  eq("emacsclient", cmd, "emacsclient -t +3:4 '/a/b.c'");
  openpath_command(NULL, "hx", "/a/b.c", 12, 5, cmd, sizeof cmd);
  eq("helix", cmd, "hx '/a/b.c:12:5'");
  openpath_command(NULL, "code -w", "/a/b.c", 12, 0, cmd, sizeof cmd);
  eq("vscode", cmd, "code -w --goto '/a/b.c:12'");
  openpath_command(NULL, "ed", "/a/b.c", 12, 0, cmd, sizeof cmd);
  eq("an editor nobody listed gets the path", cmd, "ed '/a/b.c'");
  openpath_command(NULL, "nvim", "/a/b.c", 0, 0, cmd, sizeof cmd);
  eq("no line, no line argument", cmd, "nvim '/a/b.c'");
  openpath_command(NULL, NULL, "/a/b.c", 0, 0, cmd, sizeof cmd);
  eq("no editor is vi", cmd, "vi '/a/b.c'");
  openpath_command(NULL, "vi", "/it's.c", 0, 0, cmd, sizeof cmd);
  eq("a quote in the name", cmd, "vi '/it'\\''s.c'");
  openpath_command("nvim --server x --remote-send ':e {path}<CR>{line}G'", NULL,
                   "/a/b.c", 9, 0, cmd, sizeof cmd);
  eq("a template", cmd, "nvim --server x --remote-send ':e '/a/b.c'<CR>9G'");
  openpath_command("cd {dir} && less", NULL, "/a/b.c", 0, 0, cmd, sizeof cmd);
  eq("a template without {path} gets it appended", cmd,
     "cd '/a' && less '/a/b.c'");
  ok("too small says so",
     !openpath_command(NULL, "nvim", "/a/b.c", 0, 0, cmd, 8), "");

  printf("%s\n", fails ? "FAILED" : "all ok");
  return fails ? 1 : 0;
}
