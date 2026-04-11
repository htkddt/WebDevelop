/* Unit tests: vector_generate_custom — deterministic dim, stable across calls, different texts differ. */
#include "../include/vector_generate.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failed;

static void ok(int cond, const char *name) {
    if (cond)
        printf("  [OK] %s\n", name);
    else {
        printf("  [FAIL] %s\n", name);
        failed = 1;
    }
}

static double l2norm(const float *v, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; i++) s += (double)v[i] * (double)v[i];
    return sqrt(s);
}

int main(void) {
    printf("=== vector_generate_custom ===\n");
    float a[1024], b[1024], c[1024];
    size_t da = 0, db = 0, dc = 0;

    ok(vector_generate_custom("hello world", a, sizeof(a) / sizeof(a[0]), &da) == 0, "returns 0");
    ok(da == VECTOR_GEN_CUSTOM_DIM, "out_dim == VECTOR_GEN_CUSTOM_DIM");
    ok(fabs(l2norm(a, da) - 1.0) < 1e-4, "L2 norm ~1");

    ok(vector_generate_custom("hello world", b, sizeof(b) / sizeof(b[0]), &db) == 0, "second call ok");
    ok(db == da, "same dim");
    ok(memcmp(a, b, da * sizeof(float)) == 0, "deterministic identical text");

    ok(vector_generate_custom("goodbye moon", c, sizeof(c) / sizeof(c[0]), &dc) == 0, "third text ok");
    ok(memcmp(a, c, da * sizeof(float)) != 0, "different text -> different vector");

    ok(vector_generate_custom("", a, sizeof(a) / sizeof(a[0]), &da) == 0, "empty string ok");
    ok(fabs(l2norm(a, da) - 1.0) < 1e-4, "empty -> still unit vector");

    ok(vector_generate_custom("test", a, VECTOR_GEN_CUSTOM_DIM - 1, &da) != 0, "max_dim too small -> error");

    return failed ? 1 : 0;
}
