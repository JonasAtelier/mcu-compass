/*
 * The LIS3MDL tables and conversions against ST's datasheet, DocID024204
 * Rev 4, typed in again here from the tables rather than copied from the
 * header, so a slip in one shows up as a mismatch.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "lis3mdl.h"
#include "lis3mdl_math.h"
#include "lis3mdl_regs.h"

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-3f;
}

static void test_tables(void)
{
	/* Table 3 FS and GN, Table 25 codes. */
	static const int fs[] = { 4, 8, 12, 16 };
	static const int gn[] = { 6842, 3421, 2281, 1711 };
	/* Table 22. */
	static const float odr[] = { 0.625f, 1.25f, 2.5f, 5, 10, 20, 40, 80 };

	for (int i = 0; i < 4; i++) {
		assert(lis3mdl_fs_gauss(i) == fs[i]);
		assert(lis3mdl_lsb_per_gauss(i) == gn[i]);
		/* Full scale fits int16 on every range. */
		assert(fs[i] * gn[i] < 32767);
	}
	assert(lis3mdl_fs_gauss(-1) == 0 && lis3mdl_fs_gauss(4) == 0);
	assert(lis3mdl_lsb_per_gauss(4) == 0);
	for (int i = 0; i < 8; i++)
		assert(near(lis3mdl_odr_hz(i), odr[i]));
	assert(lis3mdl_odr_hz(8) == 0.0f && lis3mdl_odr_hz(-1) == 0.0f);

	/* Range and performance enums are the register codes; ODR's maps. */
	assert(LIS3MDL_RANGE_400UT == 0 && LIS3MDL_RANGE_1600UT == 3);
	assert(LIS3MDL_LOW_POWER == 0 && LIS3MDL_ULTRA_HIGH == 3);
	assert(near(lis3mdl_odr_hz(lis3mdl_do_code(LIS3MDL_ODR_10HZ)), 10));
	assert(near(lis3mdl_odr_hz(lis3mdl_do_code(LIS3MDL_ODR_0_625HZ)),
		    0.625f));
	assert(near(lis3mdl_odr_hz(lis3mdl_do_code(LIS3MDL_ODR_5HZ)), 5));
	assert(near(lis3mdl_odr_hz(lis3mdl_do_code(LIS3MDL_ODR_20HZ)), 20));
	assert(near(lis3mdl_odr_hz(lis3mdl_do_code(LIS3MDL_ODR_80HZ)), 80));
	assert(lis3mdl_do_code(-1) == -1 && lis3mdl_do_code(8) == -1);
	/* Table 16: CTRL_REG1 resets to 00010000b, DO 100b, 10 Hz. */
	assert(lis3mdl_do_code(0) << LIS3MDL_DO_SHIFT == 0x10);
}

static void test_conversions(void)
{
	static const uint8_t le[2] = { 0x34, 0x12 };
	static const uint8_t neg[2] = { 0xFF, 0xFF };
	int16_t raw[3] = { 0, 0, 0 };

	/* One gauss of counts is 100 uT, on every range. */
	for (int i = 0; i < 4; i++) {
		assert(near(lis3mdl_ut((int16_t)lis3mdl_lsb_per_gauss(i), i),
			    100.0f));
		assert(near(lis3mdl_ut((int16_t)-lis3mdl_lsb_per_gauss(i), i),
			    -100.0f));
	}

	/* §7.8: BLE 0, low byte first, two's complement. */
	assert(lis3mdl_sample(le) == 0x1234);
	assert(lis3mdl_sample(neg) == -1);

	/* Clipped past FS x GN, not at it, on each axis and either sign. */
	for (int axis = 0; axis < 3; axis++) {
		raw[0] = raw[1] = raw[2] = 0;
		raw[axis] = 4 * 6842;
		assert(!lis3mdl_clipped(raw, 0));
		raw[axis] = 4 * 6842 + 1;
		assert(lis3mdl_clipped(raw, 0));
		raw[axis] = -(4 * 6842 + 1);
		assert(lis3mdl_clipped(raw, 0));
	}
	raw[0] = 16 * 1711;
	raw[1] = raw[2] = 0;
	assert(!lis3mdl_clipped(raw, 3));

	/* Table 4: 8 LSB per degree. */
	assert(near(lis3mdl_temp_c(80), 10.0f));
	assert(near(lis3mdl_temp_c(-4), -0.5f));
}

static void test_registers(void)
{
	/* Table 16 addresses, Table 17 identity. */
	assert(LIS3MDL_WHO_AM_I == 0x0F && LIS3MDL_WHO_AM_I_VALUE == 0x3D);
	assert(LIS3MDL_CTRL_REG1 == 0x20 && LIS3MDL_CTRL_REG2 == 0x21 &&
	       LIS3MDL_CTRL_REG3 == 0x22 && LIS3MDL_CTRL_REG4 == 0x23 &&
	       LIS3MDL_CTRL_REG5 == 0x24);
	assert(LIS3MDL_STATUS_REG == 0x27 && LIS3MDL_OUT_X_L == 0x28 &&
	       LIS3MDL_TEMP_OUT_L == 0x2E);
	/* Table 11: 3Ch/3Dh and 38h/39h are the 8-bit forms. */
	assert(LIS3MDL_ADDR_HIGH << 1 == 0x3C && LIS3MDL_ADDR_LOW << 1 == 0x38);
	/* Tables 18, 23, 29, 32 and 34: bit positions. */
	assert(LIS3MDL_TEMP_EN == 0x80 && (3 << LIS3MDL_OM_SHIFT) == 0x60 &&
	       (7 << LIS3MDL_DO_SHIFT) == 0x1C);
	assert((3 << LIS3MDL_FS_SHIFT) == 0x60);
	assert((3 << LIS3MDL_OMZ_SHIFT) == 0x0C);
	assert(LIS3MDL_BDU == 0x40 && LIS3MDL_ZYXDA == 0x08);
	/* Table 28: 00 continuous, 11 power-down. */
	assert(LIS3MDL_MD_CONTINUOUS == 0 && LIS3MDL_MD_POWER_DOWN == 3);
}

int main(void)
{
	test_tables();
	test_conversions();
	test_registers();
	printf("all lis3mdl math checks passed\n");
	return 0;
}
