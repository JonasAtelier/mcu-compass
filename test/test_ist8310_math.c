/*
 * Host-side check of the IST8310 conversions: make -C test test
 *
 * Section numbers are the iSentek datasheet, Version 1.2 (docs/).
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "ist8310.h"
#include "ist8310_math.h"
#include "ist8310_regs.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4 * fabs(b) + 1e-9)

static void test_sensitivity(void)
{
	/* §4.4: RESO 0.3 uT/LSB, and SEN 3.3 LSB/uT is that within 1%. */
	assert(NEAR(ist8310_ut(1), 0.3f));
	assert(NEAR(ist8310_ut(-10), -3.0f));
	assert(fabs(1.0 / IST8310_UT_PER_LSB - 3.3) < 0.034);

	/* The earth's 25-65 uT is a few hundred counts. */
	assert(NEAR(ist8310_ut(167), 50.1f));
}

static void test_range(void)
{
	int16_t v[3] = { 0, 0, 0 };

	/* §4.4: +/-1600 uT on X and Y; one count more is out. */
	assert(ist8310_ut(IST8310_XY_LIMIT) <= 1600.0f);
	assert(ist8310_ut(IST8310_XY_LIMIT + 1) > 1600.0f);
	/* Z's +/-2500 uT is past the 14-bit output (§1): the rail limits it. */
	assert(ist8310_ut(IST8310_Z_LIMIT) < 2500.0f);
	assert(IST8310_Z_LIMIT == (1 << 13) - 1);

	assert(!ist8310_clipped(v));
	for (int axis = 0; axis < 3; axis++) {
		int limit = axis == 2 ? IST8310_Z_LIMIT : IST8310_XY_LIMIT;

		v[0] = v[1] = v[2] = 0;
		v[axis] = (int16_t)limit;
		assert(!ist8310_clipped(v));
		v[axis] = (int16_t)-limit;
		assert(!ist8310_clipped(v));
		v[axis] = (int16_t)(limit + 1);
		assert(ist8310_clipped(v));
		v[axis] = (int16_t)(-limit - 1);
		assert(ist8310_clipped(v));
	}
	/* Z may go past the X/Y limit; X may not. */
	v[0] = v[1] = 0;
	v[2] = IST8310_XY_LIMIT + 1;
	assert(!ist8310_clipped(v));
}

static void test_sample_is_little_endian(void)
{
	const uint8_t p[2] = { 0x34, 0x12 };	/* §6.4.4: low byte first */
	const uint8_t neg[2] = { 0xFF, 0xFF };

	assert(ist8310_sample(p) == 0x1234);
	assert(ist8310_sample(neg) == -1);
}

static void test_average(void)
{
	/* §6.4.10: codes 0-4 are 1, 2, 4, 8, 16; anything else is refused. */
	assert(ist8310_avg_samples(IST8310_AVG_1) == 1);
	assert(ist8310_avg_samples(IST8310_AVG_2) == 2);
	assert(ist8310_avg_samples(IST8310_AVG_4) == 4);
	assert(ist8310_avg_samples(IST8310_AVG_8) == 8);
	assert(ist8310_avg_samples(IST8310_AVG_16) == 16);
	assert(ist8310_avg_samples(5) == 0);
	assert(ist8310_avg_samples(-1) == 0);

	/* §3.1.1: 16 on both is 24h. Y is the upper field. */
	assert(ist8310_avgcntl(IST8310_AVG_16, IST8310_AVG_16) == 0x24);
	assert(ist8310_avgcntl(IST8310_AVG_2, IST8310_AVG_1) == 0x01);
	assert(ist8310_avgcntl(IST8310_AVG_1, IST8310_AVG_2) == 0x08);
}

static void test_registers(void)
{
	/* Typed from §6.4 one more time, so a slip in the header shows. */
	assert(IST8310_WAI == 0x00 && IST8310_WAI_VALUE == 0x10);
	assert(IST8310_STAT1 == 0x02 && IST8310_STAT1_DRDY == 0x01);
	assert(IST8310_DATA_X_LSB == 0x03 && IST8310_DATA_LEN == 6);
	assert(IST8310_CNTL1 == 0x0A && IST8310_MODE_SINGLE == 0x01);
	assert(IST8310_CNTL2 == 0x0B && IST8310_CNTL2_SRST == 0x01);
	assert((IST8310_CNTL2_DREN | IST8310_CNTL2_DRP) == 0x0C);
	assert(IST8310_AVGCNTL == 0x41);
	assert(IST8310_PDCNTL == 0x42 && IST8310_PDCNTL_NORMAL == 0xC0);
	/* §6.1.1: 0001 1xx, four addresses. */
	assert(IST8310_ADDR_LL == 0x0C && IST8310_ADDR_HH == 0x0F);
}

int main(void)
{
	test_sensitivity();
	test_range();
	test_sample_is_little_endian();
	test_average();
	test_registers();
	printf("all ist8310 math checks passed\n");
	return 0;
}
