#include <stdio.h>
#include "fixed_point.h"

int main(void) {
	fp_t x = fp_from_int(-2);
	fp_t y = fp_from_int(3);
	printf("%f\n", fp_to_float(fp_mul_fp(x, y)));
	printf("%d\n", fp_round(fp_div_fp(x, y)));
	return 0;
}