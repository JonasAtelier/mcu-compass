/*
 * Pure register <-> physical-unit conversions for the QMC5883P, split out so
 * they can be compiled and asserted on the host without an ARM toolchain.
 * Section and table numbers are QST-PD-B002-22 Rev E (docs/).
 */
#ifndef QMC5883P_MATH_H
#define QMC5883P_MATH_H

#include <stdint.h>

#include "qmc5883p_regs.h"

#define QMC5883P_UT_PER_GAUSS	100.0f

/*
 * Table 2 sensitivity against Table 18's RNG codes: 00 is +/-30 G at 1000
 * LSB/G, 01 +/-12 G at 2500, 10 +/-8 G at 3750, 11 +/-2 G at 15000. 0 means
 * the code is invalid.
 */
static inline float qmc5883p_lsb_per_gauss(int rng)
{
	static const float lsb[4] = { 1000.0f, 2500.0f, 3750.0f, 15000.0f };

	return (rng >= 0 && rng <= 3) ? lsb[rng] : 0.0f;
}

/* Counts per microtesla; 1 gauss is 100 uT. */
static inline float qmc5883p_lsb_per_ut(int rng)
{
	return qmc5883p_lsb_per_gauss(rng) / QMC5883P_UT_PER_GAUSS;
}

/* Full scale in microtesla, Table 18: 30, 12, 8 and 2 gauss. */
static inline float qmc5883p_fs_ut(int rng)
{
	static const float fs[4] = { 3000.0f, 1200.0f, 800.0f, 200.0f };

	return (rng >= 0 && rng <= 3) ? fs[rng] : 0.0f;
}

static inline float qmc5883p_ut(int16_t raw, int rng)
{
	float s = qmc5883p_lsb_per_ut(rng);

	return s > 0.0f ? (float)raw / s : 0.0f;
}

/* Table 17 ODR codes, in Hz. Normal mode only; continuous ignores it. */
static inline float qmc5883p_odr_hz(int odr)
{
	static const float hz[4] = { 10.0f, 50.0f, 100.0f, 200.0f };

	return (odr >= 0 && odr <= 3) ? hz[odr] : 0.0f;
}

/* Table 17 OSR1, the over sample ratio: codes run 8, 4, 2, 1. */
static inline int qmc5883p_osr1_ratio(int osr1)
{
	static const int r[4] = { 8, 4, 2, 1 };

	return (osr1 >= 0 && osr1 <= 3) ? r[osr1] : 0;
}

/* Table 17 OSR2, the down sampling rate: codes run 1, 2, 4, 8. */
static inline int qmc5883p_osr2_ratio(int osr2)
{
	static const int r[4] = { 1, 2, 4, 8 };

	return (osr2 >= 0 && osr2 <= 3) ? r[osr2] : 0;
}

/* Table 15: LSB at the lower address, two's complement. */
static inline int16_t qmc5883p_sample(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static inline uint8_t qmc5883p_ctrl1(int osr2, int osr1, int odr, int mode)
{
	return (uint8_t)((osr2 << QMC5883P_OSR2_SHIFT) |
			 (osr1 << QMC5883P_OSR1_SHIFT) |
			 (odr << QMC5883P_ODR_SHIFT) |
			 (mode << QMC5883P_MODE_SHIFT));
}

/* CTRL2 for normal running: no reset, no self-test, set and reset on. */
static inline uint8_t qmc5883p_ctrl2(int rng)
{
	return (uint8_t)((rng << QMC5883P_RNG_SHIFT) |
			 (QMC5883P_SR_ON << QMC5883P_SR_SHIFT));
}

/*
 * §7.3 step 8: the self-test response is (x1 - x2, y2 - y1, z2 - z1) -- X is
 * taken the other way round from Y and Z. before is the reading without the
 * stimulus, after the one with it.
 */
static inline void qmc5883p_self_test_delta(const int16_t before[3],
					    const int16_t after[3],
					    int16_t delta[3])
{
	delta[0] = (int16_t)(before[0] - after[0]);
	delta[1] = (int16_t)(after[1] - before[1]);
	delta[2] = (int16_t)(after[2] - before[2]);
}

#endif /* QMC5883P_MATH_H */
