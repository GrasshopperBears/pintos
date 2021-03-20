#include <stdint.h>

static int fraction = 1<<14;

int
int_to_fixed_point (int n) {
    int f = n * fraction;

    return f;
}

int
fixed_point_to_int (int f) {
    int n = f / fraction;

    return n;
}

int
round_to_nearest (int f) {
    int n;

    if (f >= 0 )
        n = ((f + fraction) / 2) / fraction;
    else
        n = ((f - fraction) / 2) / fraction;
}

int
add_f_f (int f1, int f2) {
    int f = f1 + f2;

    return f;
}

int
sub_f_f (int f1, int f2) {
    int f = f1 - f2;

    return f;
}

int
add_f_n (int f, int n) {
    int f_result = f + int_to_fixed_point (n);

    return f_result;
}

int
sub_f_n (int f, int n) {
    int f_result = f - int_to_fixed_point (n);

    return f_result;
}

int
multiple_f_f (int f1, int f2) {
    int f_result = (int64_t) f1 * f2 / fraction;

    return (int) f_result;
}

int
multiple_f_n (int f, int n) {
    int f_result = f * n;

    return f_result;
}

int
divide_f_f (int f1, int f2) {
    int f_result = (int64_t) f1 * fraction / f2;

    return (int) f_result;
}

int
divide_f_n (int f, int n) {
    int f_result = f / n;

    return f_result;
}