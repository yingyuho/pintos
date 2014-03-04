#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <inttypes.h>
#include <limits.h>

typedef int32_t fp_t;

// Number of fractional bits
#define FP_Q 14
// 2 ** FP_Q
#define FP_F (1 << FP_Q)

fp_t fp_from_int(int32_t n);

int32_t fp_floor(fp_t x);
int32_t fp_round(fp_t x);

fp_t fp_add_fp(fp_t x, fp_t y);
fp_t fp_sub_fp(fp_t x, fp_t y);
fp_t fp_mul_fp(fp_t x, fp_t y);
fp_t fp_div_fp(fp_t x, fp_t y);

fp_t fp_add_int(fp_t x, int32_t n);
fp_t fp_sub_int(fp_t x, int32_t n);
fp_t fp_mul_int(fp_t x, int32_t n);
fp_t fp_div_int(fp_t x, int32_t n);

#endif /* threads/fixed_point.h */
