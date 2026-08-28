/* Paths, as the two platforms spell them.
 *
 * Windows takes `/` and `\` and mixes them without being asked, and calls a
 * path absolute when it starts with a drive. POSIX does neither: `\` is an
 * ordinary character in a file name there, and `C:` an ordinary directory.
 * So half of what follows asserts that the Windows rules are *not* applied on
 * POSIX -- which is the half that would otherwise rot, because the platform
 * where these functions are interesting is not the platform the suite runs on.
 */
#include "slosh.h"

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(const char *name, bool cond, const char *detail) {
  if (!cond) fails++;
  printf("%s %-52s %s\n", cond ? "ok  " : "FAIL", name, cond ? "" : detail);
}

static void eq(const char *name, const char *got, const char *want) {
  bool same = got && want && strcmp(got, want) == 0;
  char detail[512];
  snprintf(detail, sizeof detail, "got %s, wanted %s", got ? got : "(null)",
           want);
  ok(name, same, detail);
}

int main(void) {
  char buf[512];

  /* ---- absoluteness ----------------------------------------------------- */
  ok("a leading slash is absolute", path_is_absolute("/etc/slosh.kdl"), "");
  ok("a bare name is not", !path_is_absolute("themes/t.kdl"), "");
  ok("a dotted name is not", !path_is_absolute("./themes/t.kdl"), "");
  ok("an empty path is not", !path_is_absolute(""), "");
  ok("NULL is not", !path_is_absolute(NULL), "");

#ifdef _WIN32
  ok("a drive with a slash is absolute", path_is_absolute("C:/themes"), "");
  ok("a drive with a backslash is absolute", path_is_absolute("C:\\themes"),
     "");
  ok("a lowercase drive counts", path_is_absolute("d:/x"), "");
  ok("a drive-relative root is absolute", path_is_absolute("\\themes"), "");
  ok("a UNC share is absolute", path_is_absolute("\\\\host\\share\\t.kdl"), "");
  /* `C:t.kdl` is relative to the current directory *of drive C*, which is a
   * real thing and not one we can resolve for anybody. */
  ok("a drive without a separator is not", !path_is_absolute("C:t.kdl"), "");
#else
  /* The whole reason the drive rules are #ifdef'd: these are ordinary
   * relative paths here, and reading them as absolute would break the
   * platform that works in order to fix the one that did not. */
  ok("a drive letter is just a name on POSIX", !path_is_absolute("C:/themes"),
     "");
  ok("a backslash is just a character on POSIX", !path_is_absolute("\\themes"),
     "");
#endif

  /* ---- splitting -------------------------------------------------------- */
  eq("the directory of a nested path", path_dir("/a/b/c.kdl", buf, sizeof buf),
     "/a/b");
  eq("the name of a nested path", path_base("/a/b/c.kdl"), "c.kdl");
  eq("a bare name is in the current directory", path_dir("c.kdl", buf,
                                                         sizeof buf),
     ".");
  eq("and is its own name", path_base("c.kdl"), "c.kdl");
  eq("a path at the root", path_dir("/c.kdl", buf, sizeof buf), "/");
  eq("its name", path_base("/c.kdl"), "c.kdl");

#ifdef _WIN32
  eq("a backslash separates", path_dir("C:\\Users\\you\\config.kdl", buf,
                                       sizeof buf),
     "C:\\Users\\you");
  eq("and names", path_base("C:\\Users\\you\\config.kdl"), "config.kdl");
  /* The mixture is not hypothetical: slosh joins with '/' and $HOME arrives
   * with '\', so the default config path is spelled both ways at once. */
  eq("the last separator wins in a mixed path",
     path_dir("C:\\Users\\you/.config/slosh/config.kdl", buf, sizeof buf),
     "C:\\Users\\you/.config/slosh");
  eq("and the other order", path_dir("C:/Users/you\\config.kdl", buf,
                                     sizeof buf),
     "C:/Users/you");
  /* `C:` alone is the current directory on drive C, which is somewhere else. */
  eq("a file at a drive root keeps the separator",
     path_dir("C:\\config.kdl", buf, sizeof buf), "C:\\");
  eq("forward-slash spelling too", path_dir("C:/config.kdl", buf, sizeof buf),
     "C:/");
#else
  /* A POSIX file may legitimately be called `a\b`; splitting there would
   * invent a directory that does not exist. */
  eq("a backslash does not split on POSIX", path_dir("a\\b", buf, sizeof buf),
     ".");
  eq("nor name", path_base("a\\b"), "a\\b");
  eq("a colon is not a drive on POSIX", path_dir("a:/b", buf, sizeof buf),
     "a:");
#endif

  /* ---- resolving against a base ----------------------------------------- */
  eq("a relative path joins the base",
     path_resolve("themes/t.kdl", "/cfg", buf, sizeof buf), "/cfg/themes/t.kdl");
  eq("an absolute path ignores the base",
     path_resolve("/etc/t.kdl", "/cfg", buf, sizeof buf), "/etc/t.kdl");
  eq("no base leaves it alone",
     path_resolve("themes/t.kdl", NULL, buf, sizeof buf), "themes/t.kdl");
  eq("`.` is the base itself", path_resolve(".", "/cfg", buf, sizeof buf),
     "/cfg");
#ifdef _WIN32
  /* The bug this was written for: joined instead of taken as it stood, and
   * opened as C:/cfg/C:/themes/t.kdl. */
  eq("a drive path ignores the base",
     path_resolve("C:/themes/t.kdl", "C:/cfg", buf, sizeof buf),
     "C:/themes/t.kdl");
#endif

  if (fails) printf("\n%d failed\n", fails);
  return fails ? 1 : 0;
}
