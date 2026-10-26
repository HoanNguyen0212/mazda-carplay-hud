#ifndef CMU_SPEED_NORMALIZATION_H
#define CMU_SPEED_NORMALIZATION_H

#include <stdint.h>

/*
 * Mazda Connect FW 74.00.324 observation:
 *   raw GetCarSpeed at rest: 10000
 *   raw moving trace:        10000..14256
 *
 * The CMU value is therefore not a direct km/h integer on this target. The
 * observed baseline is a stationary/unavailable sentinel/offset; subtract it
 * before converting the hundredths-like delta to km/h. A small direct-km/h
 * fallback is retained for older/alternate BCM implementations.
 */
#define CMU_SPEED_RAW_BASELINE 10000U
#define CMU_SPEED_MAX_KMH      260U

static inline int32_t normalize_cmu_speed_kmh(uint32_t raw)
{
    if (raw == 0U) return -1;

    /* Compatibility path for a BCM that already returns km/h directly. */
    if (raw < 300U) return (int32_t)raw;

    /* On the observed CMU, <= baseline is stationary/unavailable. */
    if (raw <= CMU_SPEED_RAW_BASELINE) return -1;

    uint32_t delta = raw - CMU_SPEED_RAW_BASELINE;
    if (delta > CMU_SPEED_MAX_KMH * 100U) return -1;

    return (int32_t)((delta + 50U) / 100U);
}

#endif
