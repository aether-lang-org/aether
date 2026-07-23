#ifndef AE_BINDGEN_H
#define AE_BINDGEN_H

// `ae bindgen consts` — import object-like C macros that expand to scalar
// constants (integers, strings, floats) as Aether consts (#1245). `cc` is
// the C compiler the driver already resolved; both pipeline stages are
// preprocessor-only, nothing is executed. Returns 0 on success.
int ae_bindgen_consts(const char* cc, int argc, char** argv);

#endif // AE_BINDGEN_H
