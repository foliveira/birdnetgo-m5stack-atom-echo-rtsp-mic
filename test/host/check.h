#pragma once
// Minimal assertion helpers for the host tests (no test framework dependency).
#include <math.h>
#include <stdio.h>

static int checkFailures = 0;
static int checkCount = 0;

#define CHECK(cond) do { checkCount++; if (!(cond)) { checkFailures++; \
    printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); } } while (0)

#define CHECK_EQ(a, b) do { checkCount++; double _a = (double)(a), _b = (double)(b); if (_a != _b) { \
    checkFailures++; printf("%s:%d: %s == %s failed (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)

#define CHECK_NEAR(a, b, tol) do { checkCount++; double _a = (double)(a), _b = (double)(b); \
    if (!(fabs(_a - _b) <= (tol))) { checkFailures++; \
    printf("%s:%d: %s ~= %s failed (%g vs %g, tol %g)\n", __FILE__, __LINE__, #a, #b, _a, _b, (double)(tol)); } } while (0)

static int checkSummary(const char* name) {
    printf("%s: %d checks, %d failed\n", name, checkCount, checkFailures);
    return checkFailures ? 1 : 0;
}
