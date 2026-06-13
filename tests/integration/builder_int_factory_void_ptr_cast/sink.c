/* C-side sink for the builder_int_factory_void_ptr_cast regression.
 * Two int-handle externs whose declared parameters are `int` exercise
 * the two missing-cast sites in the builder/ctx lowering:
 *   - make_handle() -> int  drives `void* _bcfg = factory();`
 *   - use_handle(int)        drives `fn(_builder, ...);` with _builder void*
 *
 * Pre-fix the generated C had bare int↔pointer conversions and the
 * build failed under GCC 14+/MinGW's default -Werror=int-conversion.
 * With the fix both are explicitly (void*)(intptr_t) and (int)(intptr_t)
 * casted. We don't check runtime values here — the regression is purely
 * about the generated C compiling clean under the harder warning. */

#include <stddef.h>

static int last_used = -1;

int make_handle(void) { return 7; }

void use_handle(int h) { last_used = h; }
