#include "test_harness.h"
#include "../../std/math/aether_math.h"
#include <math.h>

TEST_CATEGORY(math_sqrt_positive, TEST_CATEGORY_STDLIB) {
    double result = math_sqrt(16.0);
    ASSERT_EQ(4, (int)result);
}

TEST_CATEGORY(math_sqrt_zero, TEST_CATEGORY_STDLIB) {
    double result = math_sqrt(0.0);
    ASSERT_EQ(0, (int)result);
}

TEST_CATEGORY(math_pow_positive, TEST_CATEGORY_STDLIB) {
    double result = math_pow(2.0, 3.0);
    ASSERT_EQ(8, (int)result);
}

TEST_CATEGORY(math_pow_zero_exponent, TEST_CATEGORY_STDLIB) {
    double result = math_pow(5.0, 0.0);
    ASSERT_EQ(1, (int)result);
}

TEST_CATEGORY(math_abs_positive, TEST_CATEGORY_STDLIB) {
    int result = math_abs_int(42);
    ASSERT_EQ(42, result);
}

TEST_CATEGORY(math_abs_negative, TEST_CATEGORY_STDLIB) {
    int result = math_abs_int(-42);
    ASSERT_EQ(42, result);
}

TEST_CATEGORY(math_abs_zero, TEST_CATEGORY_STDLIB) {
    int result = math_abs_int(0);
    ASSERT_EQ(0, result);
}

TEST_CATEGORY(math_min_basic, TEST_CATEGORY_STDLIB) {
    int result = math_min_int(5, 10);
    ASSERT_EQ(5, result);
}

TEST_CATEGORY(math_min_equal, TEST_CATEGORY_STDLIB) {
    int result = math_min_int(7, 7);
    ASSERT_EQ(7, result);
}

TEST_CATEGORY(math_max_basic, TEST_CATEGORY_STDLIB) {
    int result = math_max_int(5, 10);
    ASSERT_EQ(10, result);
}

TEST_CATEGORY(math_max_equal, TEST_CATEGORY_STDLIB) {
    int result = math_max_int(7, 7);
    ASSERT_EQ(7, result);
}

TEST_CATEGORY(math_sin_zero, TEST_CATEGORY_STDLIB) {
    double result = math_sin(0.0);
    ASSERT_EQ(0, (int)result);
}

TEST_CATEGORY(math_cos_zero, TEST_CATEGORY_STDLIB) {
    double result = math_cos(0.0);
    ASSERT_EQ(1, (int)result);
}

TEST_CATEGORY(math_floor_positive, TEST_CATEGORY_STDLIB) {
    double result = math_floor(3.7);
    ASSERT_EQ(3, (int)result);
}

TEST_CATEGORY(math_floor_negative, TEST_CATEGORY_STDLIB) {
    double result = math_floor(-3.7);
    ASSERT_EQ(-4, (int)result);
}

TEST_CATEGORY(math_ceil_positive, TEST_CATEGORY_STDLIB) {
    double result = math_ceil(3.2);
    ASSERT_EQ(4, (int)result);
}

TEST_CATEGORY(math_ceil_negative, TEST_CATEGORY_STDLIB) {
    double result = math_ceil(-3.2);
    ASSERT_EQ(-3, (int)result);
}

TEST_CATEGORY(math_round_positive_up, TEST_CATEGORY_STDLIB) {
    double result = math_round(3.6);
    ASSERT_EQ(4, (int)result);
}

TEST_CATEGORY(math_round_positive_down, TEST_CATEGORY_STDLIB) {
    double result = math_round(3.4);
    ASSERT_EQ(3, (int)result);
}

TEST_CATEGORY(math_random_int_range, TEST_CATEGORY_STDLIB) {
    // Test random integer generation
    for (int i = 0; i < 10; i++) {
        int result = math_random_int(1, 10);
        ASSERT_TRUE(result >= 1 && result <= 10);
    }
}

TEST_CATEGORY(math_random_int_single, TEST_CATEGORY_STDLIB) {
    int result = math_random_int(5, 5);
    ASSERT_EQ(5, result);
}

TEST_CATEGORY(math_random_float_range, TEST_CATEGORY_STDLIB) {
    /* math_random_float returns rand()/RAND_MAX — contract is the
     * closed interval [0.0, 1.0].  Sample many times to catch a
     * stuck or out-of-range generator. */
    for (int i = 0; i < 100; i++) {
        double r = math_random_float();
        ASSERT_TRUE(r >= 0.0 && r <= 1.0);
    }
}

TEST_CATEGORY(math_random_float_varies, TEST_CATEGORY_STDLIB) {
    /* Guards against a degenerate generator that returns one fixed
     * value (the failure mode behind mquickjs's JS Math.random
     * returning a constant).  Across 50 draws at least two must
     * differ. */
    double first = math_random_float();
    int saw_different = 0;
    for (int i = 0; i < 50; i++) {
        if (math_random_float() != first) {
            saw_different = 1;
            break;
        }
    }
    ASSERT_TRUE(saw_different);
}

TEST_CATEGORY(math_random_float_seed_reproducible, TEST_CATEGORY_STDLIB) {
    /* Seeding with a fixed value must make the float stream
     * deterministic: the same seed yields the same first draw. */
    math_random_seed(12345);
    double a = math_random_float();
    math_random_seed(12345);
    double b = math_random_float();
    ASSERT_TRUE(a == b);
}
