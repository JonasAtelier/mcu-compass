/*
 * Host-side check of the QMC5883L conversions: make -C test test
 *
 * The sensitivities come from Linux's qmc5883l_scales[] as milligauss per
 * LSB; ESPHome carries the same two numbers. Both are asserted here against
 * the LSB-per-gauss form the datasheet states, so a unit slip shows up.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "qmc5883l.h"
#include "qmc5883l_math.h"
#include "qmc5883l_regs.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4 * fabs(b) + 1e-9)

static void test_sensitivity(void)
{
    /* 12000 and 3000 LSB per gauss, from the datasheet and both drivers. */
    assert(NEAR(qmc5883l_lsb_per_gauss(0), 12000.0f));
    assert(NEAR(qmc5883l_lsb_per_gauss(1), 3000.0f));
    /* Linux stores these as milligauss per LSB -- check we agree with it. */
    assert(NEAR(1000.0f / qmc5883l_lsb_per_gauss(0), 0.083333f));
    assert(NEAR(1000.0f / qmc5883l_lsb_per_gauss(1), 0.333333f));

    /* 1 gauss is 100 uT, so 12000 LSB/G is 120 LSB/uT. */
    assert(NEAR(qmc5883l_lsb_per_ut(0), 120.0f));
    assert(NEAR(qmc5883l_lsb_per_ut(1), 30.0f));

    /* The wide range is exactly 4x coarser, not 3x or 5x. */
    assert(NEAR(qmc5883l_lsb_per_ut(0), qmc5883l_lsb_per_ut(1) * 4.0f));

    /* Full scale and sensitivity must agree: the count at full scale has to
     * fit a signed 16-bit register. */
    for (int r = 0; r <= 1; r++) {
        float counts = qmc5883l_fs_ut(r) * qmc5883l_lsb_per_ut(r);
        assert(counts <= 32767.0f);
        assert(counts > 20000.0f);          /* and not waste most of the range */
    }
    assert(NEAR(qmc5883l_fs_ut(0), 200.0f));
    assert(NEAR(qmc5883l_fs_ut(1), 800.0f));

    assert(qmc5883l_lsb_per_gauss(2) == 0.0f);
    assert(qmc5883l_lsb_per_ut(-1) == 0.0f);
    assert(qmc5883l_fs_ut(2) == 0.0f);
}

static void test_conversions(void)
{
    assert(NEAR(qmc5883l_ut(120, 0), 1.0f));
    assert(NEAR(qmc5883l_ut(-120, 0), -1.0f));
    assert(NEAR(qmc5883l_ut(30, 1), 1.0f));
    assert(NEAR(qmc5883l_ut(24000, 0), 200.0f));   /* full scale at 2 G */
    assert(qmc5883l_ut(1000, 2) == 0.0f);          /* bad code, not a number */

    /* 100 LSB per degree, relative. */
    assert(NEAR(qmc5883l_temp_c(100), 1.0f));
    assert(NEAR(qmc5883l_temp_c(-2500), -25.0f));
    assert(NEAR(qmc5883l_temp_c(0), 0.0f));
}

static void test_rates(void)
{
    assert(NEAR(qmc5883l_odr_hz(0), 10.0f));
    assert(NEAR(qmc5883l_odr_hz(3), 200.0f));
    assert(qmc5883l_odr_hz(4) == 0.0f);
    assert(qmc5883l_odr_hz(-1) == 0.0f);

    /* Oversampling codes run backwards: 0 is the most averaging. */
    assert(qmc5883l_osr_ratio(0) == 512);
    assert(qmc5883l_osr_ratio(3) == 64);
    assert(qmc5883l_osr_ratio(0) > qmc5883l_osr_ratio(3));
    assert(qmc5883l_osr_ratio(4) == 0);
}

static void test_sample_is_little_endian(void)
{
    const uint8_t p[2] = { 0x34, 0x12 };     /* low byte first */
    const uint8_t neg[2] = { 0xFF, 0xFF };
    const uint8_t min[2] = { 0x00, 0x80 };
    const uint8_t max[2] = { 0xFF, 0x7F };

    assert(qmc5883l_sample(p) == 0x1234);
    assert(qmc5883l_sample(neg) == -1);
    assert(qmc5883l_sample(min) == -32768);
    assert(qmc5883l_sample(max) == 32767);
}

static void test_register_constants(void)
{
    /* The public enums are the register codes the maths indexes by. */
    assert(QMC5883L_RANGE_200UT == 0 && QMC5883L_RANGE_800UT == 1);
    assert(QMC5883L_ODR_10HZ == 0 && QMC5883L_ODR_200HZ == 3);
    assert(QMC5883L_OSR_512 == 0 && QMC5883L_OSR_64 == 3);
    assert(QMC5883L_ADDR == 0x0D);
    /* The identity value is also what an empty bus reads -- the driver has to
     * do more than check it, and this is here so nobody "fixes" the value. */
    assert(QMC5883L_CHIP_ID_VALUE == 0xFF);
    assert(QMC5883L_CHIP_ID == 0x0D);
    assert(QMC5883L_DATA_X_LSB == 0x00 && QMC5883L_DATA_LEN == 6);
    assert(QMC5883L_STATUS == 0x06 && QMC5883L_TEMP_LSB == 0x07);
    assert(QMC5883L_CTRL1 == 0x09 && QMC5883L_CTRL2 == 0x0A);
    assert(QMC5883L_SET_RESET == 0x0B && QMC5883L_SET_RESET_VAL == 0x01);
    assert(QMC5883L_RESET_WAIT_MS == 10);
    assert(QMC5883L_SOFT_RST == 0x80 && QMC5883L_INT_ENB == 0x01);
    assert(QMC5883L_STATUS_DRDY == 0x01 && QMC5883L_STATUS_OVL == 0x02);
    /* CTRL1 field shifts must not overlap. */
    assert(QMC5883L_MODE_SHIFT == 0 && QMC5883L_ODR_SHIFT == 2);
    assert(QMC5883L_RNG_SHIFT == 4 && QMC5883L_OSR_SHIFT == 6);
}

int main(void)
{
    test_sensitivity();
    test_conversions();
    test_rates();
    test_sample_is_little_endian();
    test_register_constants();
    printf("all qmc5883l math checks passed\n");
    return 0;
}
