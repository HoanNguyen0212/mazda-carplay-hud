/* Regression test for the CMU GetCarSpeed raw value on FW 74.00.324.
 * The vehicle trace showed 10000 at rest and 10000..14256 while moving.
 * Compile/run from the repository root with:
 *   cc tests/cmu_speed_normalization_test.c -o /tmp/cmu_speed_normalization_test
 */
#include "../cmu_speed_normalization.h"

#include <stdio.h>
#include <stdlib.h>

static void expect_speed(unsigned int raw, int expected)
{
    int got = normalize_cmu_speed_kmh(raw);
    if (got != expected) {
        fprintf(stderr, "raw=%u: expected %d, got %d\n", raw, expected, got);
        exit(1);
    }
}

int main(void)
{
    /* 10000 is the stationary/sentinel baseline observed in live raw traces. */
    expect_speed(0, -1);
    expect_speed(100, 100);       /* compatibility fallback for direct-km/h APIs */
    expect_speed(9999, -1);
    expect_speed(10000, -1);
    expect_speed(10011, 0);
    expect_speed(10500, 5);
    expect_speed(14256, 43);
    expect_speed(15000, 50);
    expect_speed(36000, 260);
    expect_speed(36001, -1);
    puts("cmu speed normalization regression: PASS");
    return 0;
}
