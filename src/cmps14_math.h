/*
 * Pure conversions for the CMPS14's serial answers, split out so they can be
 * compiled and asserted on the host. Pages are docs/cmps14.pdf.
 */
#ifndef CMPS14_MATH_H
#define CMPS14_MATH_H

#include <stdint.h>

/* p.3 and p.6: uTesla in Q point 4, so 16 counts per uT. */
#define CMPS14_COUNTS_PER_UT	16.0f

/* p.6: every 16-bit answer is high byte first. */
static inline int16_t cmps14_sample(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline float cmps14_ut(int16_t raw)
{
	return (float)raw / CMPS14_COUNTS_PER_UT;
}

/* p.2: 0-3599, divided by 10 for 0-359.9 degrees. */
static inline float cmps14_bearing(uint16_t raw)
{
	return (float)raw / 10.0f;
}

#endif /* CMPS14_MATH_H */
