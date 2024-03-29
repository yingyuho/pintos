#include "threads/fixed_point.h"

fp_t fp_from_int(int32_t n) { return n << FP_Q; }

int32_t fp_floor(fp_t x) { return x / FP_F; }

int32_t fp_round(fp_t x) {
    int32_t sign = +1 | (x >> (sizeof(fp_t) * CHAR_BIT - 1));
    return (x + sign * FP_F / 2) / FP_F;
}

fp_t fp_add_fp(fp_t x, fp_t y) { return x + y; }

fp_t fp_sub_fp(fp_t x, fp_t y) { return x - y; }

fp_t fp_mul_fp(fp_t x, fp_t y) { return ((int64_t) x) * y / FP_F; }

fp_t fp_div_fp(fp_t x, fp_t y) { return ((int64_t) x) * FP_F / y; }

fp_t fp_add_int(fp_t x, int32_t n) { return x + n * FP_F; }

fp_t fp_sub_int(fp_t x, int32_t n) { return x - n * FP_F; }

fp_t fp_mul_int(fp_t x, int32_t n) { return x * n; }

fp_t fp_div_int(fp_t x, int32_t n) { return x / n; }