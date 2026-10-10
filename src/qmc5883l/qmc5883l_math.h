/*
 * Pure register <-> physical-unit conversions, split out so they can be
 * compiled and asserted on the host without an ARM toolchain.
 *
 * Sensitivities are from the Linux driver's qmc5883l_scales[], which carries
 * them as milligauss per LSB: 0.083333 at the 2 G range and 0.333333 at 8 G.
 * That is 12000 and 3000 LSB per gauss. ESPHome uses the same two numbers.
 *
 * The heading that used to live here is compass_heading_deg() in the core.
 */
#ifndef QMC5883L_MATH_H
#define QMC5883L_MATH_H

#include <stdint.h>

/* 1 gauss is 100 microtesla, so LSB/gauss / 100 is LSB per microtesla. */
#define QMC5883L_LSB_PER_GAUSS_2G	12000.0f
#define QMC5883L_LSB_PER_GAUSS_8G	3000.0f
#define QMC5883L_UT_PER_GAUSS		100.0f

/* Temperature is 100 LSB per degree and RELATIVE -- the offset is not
 * calibrated in the factory, so this tracks change, not absolute degrees. */
#define QMC5883L_TEMP_LSB_C		100.0f

static inline float qmc5883l_lsb_per_gauss(int rng)
{
	if (rng == 0)
		return QMC5883L_LSB_PER_GAUSS_2G;
	if (rng == 1)
		return QMC5883L_LSB_PER_GAUSS_8G;
	return 0.0f;
}

/* Counts per microtesla for a range code. 0 means the code is invalid. */
static inline float qmc5883l_lsb_per_ut(int rng)
{
	float g = qmc5883l_lsb_per_gauss(rng);

	return g > 0.0f ? g / QMC5883L_UT_PER_GAUSS : 0.0f;
}

/* Full scale in microtesla: 2 G is 200 uT, 8 G is 800 uT. */
static inline float qmc5883l_fs_ut(int rng)
{
	if (rng == 0)
		return 200.0f;
	if (rng == 1)
		return 800.0f;
	return 0.0f;
}

static inline float qmc5883l_ut(int16_t raw, int rng)
{
	float s = qmc5883l_lsb_per_ut(rng);

	return s > 0.0f ? (float)raw / s : 0.0f;
}

static inline float qmc5883l_temp_c(int16_t raw)
{
	return (float)raw / QMC5883L_TEMP_LSB_C;
}

/* Output data rate for a code, in Hz. */
static inline float qmc5883l_odr_hz(int odr)
{
	static const float hz[4] = { 10.0f, 50.0f, 100.0f, 200.0f };

	return (odr >= 0 && odr <= 3) ? hz[odr] : 0.0f;
}

/* Oversampling ratio for a code. Higher averages more and costs current. */
static inline int qmc5883l_osr_ratio(int osr)
{
	static const int r[4] = { 512, 256, 128, 64 };

	return (osr >= 0 && osr <= 3) ? r[osr] : 0;
}

/* Little endian, low byte first -- the opposite of most of this family. */
static inline int16_t qmc5883l_sample(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

#endif /* QMC5883L_MATH_H */
