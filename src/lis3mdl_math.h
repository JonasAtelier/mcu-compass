/*
 * Pure register <-> physical-unit conversions for the LIS3MDL, split out so
 * they can be compiled and asserted on the host without a toolchain. Table
 * and section numbers are ST's datasheet, DocID024204 Rev 4 (docs/).
 */
#ifndef LIS3MDL_MATH_H
#define LIS3MDL_MATH_H

#include <stdbool.h>
#include <stdint.h>

#include "lis3mdl_regs.h"

#define LIS3MDL_UT_PER_GAUSS	100.0f

/* Table 3 FS and Table 25: full scale in gauss by FS code; 0 if invalid. */
static inline int lis3mdl_fs_gauss(int code)
{
	static const int fs[] = { 4, 8, 12, 16 };

	return (code >= 0 && code <= LIS3MDL_FIELD_CODE_MAX) ? fs[code] : 0;
}

/* Table 3 GN: sensitivity in LSB/gauss by FS code; 0 if invalid. */
static inline int lis3mdl_lsb_per_gauss(int code)
{
	static const int gn[] = { 6842, 3421, 2281, 1711 };

	return (code >= 0 && code <= LIS3MDL_FIELD_CODE_MAX) ? gn[code] : 0;
}

/*
 * enum lis3mdl_odr -> Table 22's DO code, -1 if invalid. The enum puts the
 * chip's default, 10 Hz (DO 100b, Table 16), first.
 */
static inline int lis3mdl_do_code(int odr)
{
	static const int code[] = { 4, 0, 1, 2, 3, 5, 6, 7 };

	return (odr >= 0 && odr <= LIS3MDL_DO_CODE_MAX) ? code[odr] : -1;
}

/* Table 22: output data rate in Hz by DO code; 0 if invalid. */
static inline float lis3mdl_odr_hz(int code)
{
	static const float odr[] = { 0.625f, 1.25f, 2.5f, 5.0f,
				     10.0f, 20.0f, 40.0f, 80.0f };

	return (code >= 0 && code <= LIS3MDL_DO_CODE_MAX) ? odr[code] : 0.0f;
}

static inline float lis3mdl_ut(int16_t raw, int fs_code)
{
	return (float)raw * LIS3MDL_UT_PER_GAUSS /
	       (float)lis3mdl_lsb_per_gauss(fs_code);
}

/* §7.8-§7.11: low byte at the lower address with BLE 0, two's complement. */
static inline int16_t lis3mdl_sample(const uint8_t *p)
{
	return (int16_t)(((uint16_t)p[1] << 8) | p[0]);
}

/*
 * True if any axis reads past Table 3's measurement range, FS times GN: about
 * 27370 counts on every range. The part's own overflow flag is INT_SRC's
 * MROI (Table 38), in the interrupt block this driver leaves off, and the
 * datasheet does not say it updates with interrupts disabled -- so this is
 * the clipping check.
 */
static inline bool lis3mdl_clipped(const int16_t raw[3], int fs_code)
{
	int32_t limit = (int32_t)lis3mdl_fs_gauss(fs_code) *
			lis3mdl_lsb_per_gauss(fs_code);

	for (int i = 0; i < 3; i++)
		if (raw[i] > limit || raw[i] < -limit)
			return true;
	return false;
}

/*
 * Table 4: TSDr, 8 LSB/degC. The datasheet gives no zero point, so like the
 * QMC5883L this is a temperature relative to an uncalibrated zero.
 */
static inline float lis3mdl_temp_c(int16_t raw)
{
	return (float)raw / 8.0f;
}

#endif /* LIS3MDL_MATH_H */
