/* C-side closure-retention sink for the
 * closure_extern_retains_no_uaf regression. Models the canonical
 * "callback registry" extern callsite: register_cb() stores a boxed
 * closure pointer; fire_cb() invokes it later. With the pre-fix
 * codegen, the closure's heap env was freed at the call site of
 * register_cb(), so fire_cb() read freed memory (UAF). With the
 * fix, the env survives — extern callee args are conservatively
 * treated as escaping. */

typedef struct {
    void (*fn)(void);
    void* env;
} AeClosure;

static AeClosure saved;

void register_cb(AeClosure boxed) {
    saved = boxed;
}

void fire_cb(void) {
    if (saved.fn) {
        ((void (*)(void*))saved.fn)(saved.env);
    }
}
