/* Shim for the extern-tuple-VALUE pass-through regression test (#1062).
 * Each function takes or returns a struct-by-value whose layout matches the
 * codegen-synthesized `_tuple_*` typedef for the extern's tuple type. Field
 * names are `_0`, `_1`, ... in declaration order.
 *
 *   (int, int)               -> _tuple_int_int
 *   (byte, byte, byte, byte) -> _tuple_byte_byte_byte_byte   (raylib Color shape)
 */

typedef struct { int _0; int _1; } _tuple_int_int;
typedef struct { unsigned char _0; unsigned char _1;
                 unsigned char _2; unsigned char _3; } _tuple_byte_byte_byte_byte;

/* Produce a point; the probe hands the returned struct straight back to
 * pt_sum both via a variable and via a direct call chain. */
_tuple_int_int make_pt(void) {
    _tuple_int_int t = { 10, 20 };
    return t;
}

int pt_sum(_tuple_int_int p) {
    return p._0 + p._1;
}

_tuple_byte_byte_byte_byte make_color(void) {
    _tuple_byte_byte_byte_byte c = { 16, 32, 64, 127 };
    return c;
}

int color_pack(_tuple_byte_byte_byte_byte c) {
    return ((int)c._0 << 24) | ((int)c._1 << 16) | ((int)c._2 << 8) | (int)c._3;
}
