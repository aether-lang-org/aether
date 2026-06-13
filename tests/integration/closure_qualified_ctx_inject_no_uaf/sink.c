/* C-side retention sink for the closure_qualified_ctx_inject_no_uaf
 * regression. The visible-body Aether wrapper (uikit.register_btn,
 * called as qualified `uikit.register_btn`) boxes the closure and
 * hands the box to aether_ui_button_create, which stashes it.
 * fire_btn() invokes the stashed closure later. Pre-fix the call
 * site of register_btn(...) emitted `free(_ad_0.env)` so the C-side
 * invocation read freed memory. */
#include <stddef.h>

typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static AeClosure saved;

int aether_ui_button_create(const char* label, void* boxed) {
    (void)label;
    saved = *(AeClosure*)boxed;
    return 1;
}

void fire_btn(void) {
    if (saved.fn) {
        ((void (*)(void*))saved.fn)(saved.env);
    }
}
