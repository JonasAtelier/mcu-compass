/*
 * Host-side check of the QMC5883P conversions and register constants against
 * the datasheet, QST-PD-B002-22 Rev E (docs/). The literals here are typed
 * from the datasheet, not from the headers, so a header typo fails.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "qmc5883p.h"
#include "qmc5883p_math.h"
#include "qmc5883p_regs.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4 * fabs(b) + 1e-9)

static void test_sensitivity(void)
{
	/* Table 2, in Table 18's RNG code order: 30, 12, 8, 2 gauss. */
	assert(NEAR(qmc5883p_lsb_per_gauss(QMC5883P_RANGE_3000UT), 1000.0f));
	assert(NEAR(qmc5883p_lsb_per_gauss(QMC5883P_RANGE_1200UT), 2500.0f));
	assert(NEAR(qmc5883p_lsb_per_gauss(QMC5883P_RANGE_800UT), 3750.0f));
	assert(NEAR(qmc5883p_lsb_per_gauss(QMC5883P_RANGE_200UT), 15000.0f));

	/* 1 gauss is 100 uT. */
	assert(NEAR(qmc5883p_lsb_per_ut(0), 10.0f));
	assert(NEAR(qmc5883p_lsb_per_ut(3), 150.0f));

	/* Table 2 digital resolution at 30 G: 1.0 mG per LSB. */
	assert(NEAR(1000.0f / qmc5883p_lsb_per_gauss(0), 1.0f));

	/*
	 * Full scale times sensitivity is 30000 counts on every range -- the
	 * same 30000 that sets OVFL (§9.2.2), and inside the int16 output.
	 */
	for (int r = 0; r <= 3; r++) {
		float counts = qmc5883p_fs_ut(r) * qmc5883p_lsb_per_ut(r);

		assert(NEAR(counts, (float)QMC5883P_OVFL_COUNTS));
		assert(counts <= 32767.0f);
	}
	assert(QMC5883P_OVFL_COUNTS == 30000);

	assert(qmc5883p_lsb_per_gauss(4) == 0.0f);
	assert(qmc5883p_lsb_per_gauss(-1) == 0.0f);
	assert(qmc5883p_lsb_per_ut(4) == 0.0f);
	assert(qmc5883p_fs_ut(4) == 0.0f);
}

static void test_conversions(void)
{
	assert(NEAR(qmc5883p_ut(150, 3), 1.0f));
	assert(NEAR(qmc5883p_ut(-150, 3), -1.0f));
	assert(NEAR(qmc5883p_ut(10, 0), 1.0f));
	assert(NEAR(qmc5883p_ut(30000, 3), 200.0f));	/* full scale at 2 G */
	assert(NEAR(qmc5883p_ut(-30000, 0), -3000.0f));	/* and at 30 G */
	assert(qmc5883p_ut(1000, 4) == 0.0f);		/* bad code, not a number */
}

static void test_rates(void)
{
	/* Table 17 ODR: 10, 50, 100, 200 Hz. */
	assert(NEAR(qmc5883p_odr_hz(0), 10.0f));
	assert(NEAR(qmc5883p_odr_hz(1), 50.0f));
	assert(NEAR(qmc5883p_odr_hz(2), 100.0f));
	assert(NEAR(qmc5883p_odr_hz(3), 200.0f));
	assert(qmc5883p_odr_hz(4) == 0.0f && qmc5883p_odr_hz(-1) == 0.0f);

	/* OSR1 runs 8, 4, 2, 1 and OSR2 runs the other way, 1, 2, 4, 8. */
	assert(qmc5883p_osr1_ratio(0) == 8 && qmc5883p_osr1_ratio(3) == 1);
	assert(qmc5883p_osr2_ratio(0) == 1 && qmc5883p_osr2_ratio(3) == 8);
	assert(qmc5883p_osr1_ratio(4) == 0 && qmc5883p_osr2_ratio(4) == 0);
}

static void test_sample_is_little_endian(void)
{
	const uint8_t p[2] = { 0x34, 0x12 };	/* LSB at the lower address */
	const uint8_t neg[2] = { 0xFF, 0xFF };
	const uint8_t min[2] = { 0x00, 0x80 };
	const uint8_t max[2] = { 0xFF, 0x7F };

	assert(qmc5883p_sample(p) == 0x1234);
	assert(qmc5883p_sample(neg) == -1);
	assert(qmc5883p_sample(min) == -32768);
	assert(qmc5883p_sample(max) == 32767);
}

/* §7's own register values must come out of the field builders. */
static void test_datasheet_examples(void)
{
	/* §7.1: 0BH = 0x08, set/reset on, 8 gauss. */
	assert(qmc5883p_ctrl2(QMC5883P_RANGE_800UT) == 0x08);
	/* §7.1: 0AH = 0xCD, normal mode at 200 Hz (OSR2 8, OSR1 8). */
	assert(qmc5883p_ctrl1(QMC5883P_OSR2_8, QMC5883P_OSR1_8,
			      QMC5883P_ODR_200HZ, QMC5883P_MODE_NORMAL) == 0xCD);
	/* §7.2: 0AH = 0xC3, continuous. */
	assert(qmc5883p_ctrl1(QMC5883P_OSR2_8, QMC5883P_OSR1_8,
			      QMC5883P_ODR_10HZ,
			      QMC5883P_MODE_CONTINUOUS) == 0xC3);
	/* §7.3 step 2: 0AH = 0x03; §7.4: 0AH = 0x00 is suspend. */
	assert(qmc5883p_ctrl1(0, 0, 0, QMC5883P_MODE_CONTINUOUS) ==
	       QMC5883P_SELF_TEST_CTRL1);
	assert(qmc5883p_ctrl1(0, 0, 0, QMC5883P_MODE_SUSPEND) == 0x00);
	assert(QMC5883P_SUSPEND_CTRL1 == 0x00);
	/* Every range keeps set/reset on and the reset/self-test bits clear. */
	for (int r = 0; r <= 3; r++)
		assert((qmc5883p_ctrl2(r) & 0xC3) == 0);
	assert(qmc5883p_ctrl2(QMC5883P_RANGE_200UT) == 0x0C);
}

static void test_self_test_signs(void)
{
	/* §7.3 step 8: (x1 - x2), (y2 - y1), (z2 - z1). */
	const int16_t before[3] = { 100, 200, -300 };
	const int16_t after[3] = { 40, 260, -200 };
	int16_t d[3];

	qmc5883p_self_test_delta(before, after, d);
	assert(d[0] == 60 && d[1] == 60 && d[2] == 100);
}

static void test_register_constants(void)
{
	/* §5.4, Table 14-18 and §7, as printed. */
	assert(QMC5883P_ADDR == 0x2C);
	assert(QMC5883P_CHIP_ID == 0x00 && QMC5883P_CHIP_ID_VALUE == 0x80);
	assert(QMC5883P_DATA_X_LSB == 0x01 && QMC5883P_DATA_LEN == 6);
	assert(QMC5883P_STATUS == 0x09);
	assert(QMC5883P_STATUS_DRDY == 0x01 && QMC5883P_STATUS_OVFL == 0x02);
	assert(QMC5883P_CTRL1 == 0x0A && QMC5883P_CTRL2 == 0x0B);
	assert(QMC5883P_SOFT_RST == 0x80 && QMC5883P_SELF_TEST == 0x40);
	assert(QMC5883P_SELF_TEST_CTRL2 == 0x40);
	assert(QMC5883P_AXIS_SIGN == 0x29 && QMC5883P_AXIS_SIGN_VALUE == 0x06);
	assert(QMC5883P_MODE_SUSPEND == 0 && QMC5883P_MODE_NORMAL == 1 &&
	       QMC5883P_MODE_SINGLE == 2 && QMC5883P_MODE_CONTINUOUS == 3);
	assert(QMC5883P_RESET_WAIT_US == 250);
	assert(QMC5883P_SELF_TEST_WAIT_MS == 5);
	/* The public enums are the register codes. */
	assert(QMC5883P_RANGE_3000UT == 0 && QMC5883P_RANGE_200UT == 3);
	assert(QMC5883P_ODR_10HZ == 0 && QMC5883P_ODR_200HZ == 3);
	assert(QMC5883P_OSR1_8 == 0 && QMC5883P_OSR1_1 == 3);
	assert(QMC5883P_OSR2_1 == 0 && QMC5883P_OSR2_8 == 3);
}

int main(void)
{
	test_sensitivity();
	test_conversions();
	test_rates();
	test_sample_is_little_endian();
	test_datasheet_examples();
	test_self_test_signs();
	test_register_constants();
	printf("all qmc5883p math checks passed\n");
	return 0;
}
