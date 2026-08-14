/* Fixture for test_shader_plugin.py: a shader plugin with nothing to it.
 *
 * Built twice — once as itself, once with -DBAD_ABI to produce a library that
 * announces a version we do not speak, which is the case that has to fail
 * safely rather than crash.
 */
#include "shader_abi.h"

#ifdef BAD_ABI
#undef SL0PPTY_SHADER_ABI
#define SL0PPTY_SHADER_ABI 9999
#endif

/* Paints every cell's background the configured colour, at full strength when
 * amount is 255. Deterministic and position-independent, so the test can
 * assert one cell and mean it. */
static void sh_fill(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  (void)ctx;
  if (!sh->amount) return;
  c->bg = sh->color;
}

/* Uses param, so the test can prove a plugin sees the config's number. */
static void sh_stripe(const shader_t *sh, const shade_ctx_t *ctx, cell_t *c) {
  uint16_t n = sh->param ? sh->param : 2;
  if (ctx->x % n) return;
  c->bg = sh->color;
}

static const shader_def_t SHADERS[] = {
    {"testfill", sh_fill},
    {"teststripe", sh_stripe},
};

SL0PPTY_SHADER_PLUGIN("test-fixture", SHADERS)
