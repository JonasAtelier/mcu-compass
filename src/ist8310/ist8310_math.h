/*
 * Pure register <-> physical-unit conversions for the IST8310, split out so
 * they can be compiled and asserted on the host without an ARM toolchain.
 * Section numbers are the iSentek datasheet, Version 1.2 (docs/).
 */
#ifndef IST8310_MATH_H
#define IST8310_MATH_H

#include <stdbool.h>
#include <stdint.h>

#include "ist8310_regs.h"

/*
 * §4.4: RESO 0.3 uT/LSB. The same table's SEN, 3.3 LSB/uT, is that figure
 * rounded; the driver uses RESO.
 */
#define IST8310_UT_PER_LSB	0.3f

/*
 * §4.4 dynamic range, +/-1600 uT on X and Y, in counts. Z's +/-2500 uT would
 * be 8333 counts, past §1's 14-bit output, so Z is limited by the output
 * itself: 8191 is its largest positive code.
 */
#define IST8310_XY_LIMIT	5333
#define IST8310_Z_LIMIT		8191

static inline float ist8310_ut(int16_t raw)
{
	return (float)raw * IST8310_UT_PER_LSB;
}

/* §6.4.4: low byte at the lower address, two's complement. */
static inline int16_t ist8310_sample(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

/*
 * True if any axis is outside what §4.4 promises to measure. The part has no
 * overflow flag, so this is the only clipping check there is.
 */
static inline bool ist8310_clipped(const int16_t raw[3])
{
	return raw[0] > IST8310_XY_LIMIT || raw[0] < -IST8310_XY_LIMIT ||
	       raw[1] > IST8310_XY_LIMIT || raw[1] < -IST8310_XY_LIMIT ||
	       raw[2] > IST8310_Z_LIMIT || raw[2] < -IST8310_Z_LIMIT;
}

/* §6.4.10 average codes 0-4: 1, 2, 4, 8, 16 samples. 0 for anything else. */
static inline int ist8310_avg_samples(int code)
{
	return (code >= 0 && code <= IST8310_AVG_CODE_MAX) ? 1 << code : 0;
}

static inline uint8_t ist8310_avgcntl(int avg_xz, int avg_y)
{
	return (uint8_t)((avg_y << IST8310_AVG_Y_SHIFT) |
			 (avg_xz << IST8310_AVG_XZ_SHIFT));
}

#endif /* IST8310_MATH_H */
