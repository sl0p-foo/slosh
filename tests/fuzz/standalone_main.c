/* A main() for running fuzz targets without libFuzzer: replays each file
 * named on the command line (or stdin if none) through the target once.
 * This is how a crashing input gets reproduced under a debugger, and how
 * the corpus doubles as a regression suite in CI without clang's fuzzer
 * runtime. Same contract as LLVM's StandaloneFuzzTargetMain.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_stream(FILE *f, const char *name) {
  size_t cap = 1 << 16, len = 0;
  uint8_t *buf = malloc(cap);
  if (!buf) return 1;
  size_t n;
  while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
    len += n;
    if (len == cap) {
      cap *= 2;
      uint8_t *nb = realloc(buf, cap);
      if (!nb) {
        free(buf);
        return 1;
      }
      buf = nb;
    }
  }
  fprintf(stderr, "run %s (%zu bytes)\n", name, len);
  LLVMFuzzerTestOneInput(buf, len);
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) return run_stream(stdin, "<stdin>");
  for (int i = 1; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb");
    if (!f) {
      perror(argv[i]);
      return 1;
    }
    int rc = run_stream(f, argv[i]);
    fclose(f);
    if (rc) return rc;
  }
  fprintf(stderr, "ok: %d input(s)\n", argc - 1);
  return 0;
}
