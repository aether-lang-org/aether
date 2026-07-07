/* Shim for the extern-tuple-PARAM regression test (#1033) — the
 * parameter-position mirror of #271's tuple returns. Each function
 * takes a struct-by-value whose layout matches the codegen-synthesized
 * `_tuple_*` typedef for the extern's tuple parameter:
 *
 *   (f32, f32)               -> _tuple_float_float           (raylib Vector2 shape)
 *   (byte, byte, byte, byte) -> _tuple_byte_byte_byte_byte   (raylib Color shape)
 *
 * Field names are `_0`, `_1`, ... in declaration order. */

#include <stdint.h>

typedef struct { float _0; float _1; } _tuple_float_float;
typedef struct { unsigned char _0; unsigned char _1;
                 unsigned char _2; unsigned char _3; } _tuple_byte_byte_byte_byte;

/* Vector2 dot product — proves two f32-pair params arrive intact and
 * that Aether double literals were narrowed to float at the boundary. */
double vec2_dot(_tuple_float_float a, _tuple_float_float b) {
    return (double)a._0 * b._0 + (double)a._1 * b._1;
}

/* Color pack — proves 4-element byte tuples land in unsigned char
 * fields in order. Values kept below 128 so the packed result stays
 * inside a positive int32. */
int color_pack(_tuple_byte_byte_byte_byte c) {
    return ((int)c._0 << 24) | ((int)c._1 << 16) | ((int)c._2 << 8) | (int)c._3;
}

/* f32 pair in AND out — proves the f32 tuple works in both directions
 * (#1033 return-position prerequisite: raylib Vector2-returning APIs). */
_tuple_float_float vec2_swap(_tuple_float_float v) {
    _tuple_float_float r = { v._1, v._0 };
    return r;
}

/* Mixed signature — a plain pointer arg before tuple params, like
 * raylib's ImageDrawTriangle(Image*, Vector2, Vector2, Vector2, Color).
 * Returns the doubled signed area so ordering mistakes flip the sign. */
double tri_area2(void* tag, _tuple_float_float v1, _tuple_float_float v2,
                 _tuple_float_float v3) {
    (void)tag;
    return (double)(v2._0 - v1._0) * (v3._1 - v1._1)
         - (double)(v3._0 - v1._0) * (v2._1 - v1._1);
}
