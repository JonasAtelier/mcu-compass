/*
 * Host-side check of the QMC5883P driver against a fake chip behind a fake
 * HAL, ported from esp-compass's. The chip keeps the datasheet's rules
 * (QST-PD-B002-22 Rev E, docs/), and counts every time the driver breaks one:
 *
 *   - §9.2.3: suspend between any two of normal, single and continuous.
 *   - §9.2.2: reading STATUS clears DRDY and OVFL.
 *   - §6.2.3 / §7.3: self-test only in continuous mode, the stimulus lands
 *     5 ms later, SELF_TEST clears itself and the part drops to suspend.
 *   - §7: register 29H is written before any measuring mode is entered.
 *   - Table 2 / Table 18: the data registers hold the field at the RNG in
 *     CTRL2 when the measurement was taken, and keep it until the next one
 *     (§9.2.1). The field itself is in gauss, so a driver that compares two
 *     readings taken at different ranges gets caught.
 *
 * Writes to any register but CTRL1, CTRL2 and 29H trip an assert: the driver
 * has no business anywhere else.
 *
 * The clock is simulated: only HAL_Delay() moves HAL_GetTick(), by n + 1
 * ticks for HAL_Delay(n), as the real one waits, and credits the chip with
 * the n milliseconds that are sure.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass.h"
#include "qmc5883p.h"
#include "qmc5883p_math.h"
#include "qmc5883p_regs.h"

static I2C_HandleTypeDef bus;
static uint8_t regs[256];
static uint8_t chip_id;
static float field_g[3];		/* the field at the sensor, gauss */
static float stimulus_g[3];		/* self-test stimulus, gauss */
static int16_t out[3];			/* what the data registers hold */
static bool absent;			/* nothing ACKs at all */
static HAL_StatusTypeDef bus_status;	/* every transfer returns this if set */
static HAL_StatusTypeDef probe_status;	/* IsDeviceReady on a present chip */
static uint16_t dev_addr, probed_addr;	/* 7-bit */
static int allocs, frees, writes, reads, resets, status_reads, data_reads;
static int mode_violations, sign_missing, self_test_wrong_mode;
static int drdy_delay, drdy_countdown;	/* status reads before DRDY; -1 never */
static bool st_pending;
static long elapsed_ms, st_started_ms;
static uint8_t log_reg[64], log_val[64];
static int log_len;

static int mode(void)
{
	return regs[QMC5883P_CTRL1] & 0x03;
}

static void chip_soft_reset(void)
{
	memset(regs, 0, sizeof(regs));	/* suspend, §6.2.4; 29H back to 0 */
	memset(field_g, 0, sizeof(field_g));
	memset(out, 0, sizeof(out));
	st_pending = false;
}

static void chip_power_on(void)
{
	chip_soft_reset();
	chip_id = QMC5883P_CHIP_ID_VALUE;
	/* 300, 400 and 500 counts at 30 G, 1000 LSB/G. */
	stimulus_g[0] = 0.3f;
	stimulus_g[1] = 0.4f;
	stimulus_g[2] = 0.5f;
	absent = false;
	bus_status = probe_status = HAL_OK;
	dev_addr = probed_addr = 0;
	allocs = frees = writes = reads = resets = status_reads = data_reads = 0;
	mode_violations = sign_missing = self_test_wrong_mode = 0;
	drdy_delay = 1;
	drdy_countdown = -1;
	elapsed_ms = 0;
	log_len = 0;
}

/*
 * The address-only probe. The real one returns HAL_ERROR once every trial
 * went unACKed; a NACKed transfer is HAL_ERROR too, which the transfer fakes
 * below return for an absent chip.
 */
HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c,
					uint16_t address, uint32_t trials,
					uint32_t timeout)
{
	assert(hi2c == &bus);
	assert(!(address & 1) && "the HAL takes the 7-bit address << 1");
	assert(trials >= 1 && timeout > 0);
	probed_addr = address >> 1;
	if (absent)
		return HAL_ERROR;
	return probe_status;
}

static void write_ctrl1(uint8_t val)
{
	int from = mode(), to = val & 0x03;

	if (from != QMC5883P_MODE_SUSPEND && to != QMC5883P_MODE_SUSPEND)
		mode_violations++;
	if (to != QMC5883P_MODE_SUSPEND) {
		if (regs[QMC5883P_AXIS_SIGN] != QMC5883P_AXIS_SIGN_VALUE)
			sign_missing++;
		drdy_countdown = drdy_delay;
	}
	regs[QMC5883P_CTRL1] = val;
}

static void write_ctrl2(uint8_t val)
{
	if (val & QMC5883P_SOFT_RST) {
		chip_soft_reset();
		resets++;
		return;
	}
	if (val & QMC5883P_SELF_TEST) {
		if (mode() != QMC5883P_MODE_CONTINUOUS)
			self_test_wrong_mode++;
		st_pending = true;
		st_started_ms = elapsed_ms;
	}
	regs[QMC5883P_CTRL2] = val;
}

/* §8.2.3: register address, then one data byte. */
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c,
				    uint16_t address, uint16_t reg,
				    uint16_t reg_size, uint8_t *data,
				    uint16_t len, uint32_t timeout)
{
	assert(hi2c == &bus);
	assert(!(address & 1) && "the HAL takes the 7-bit address << 1");
	assert(reg_size == I2C_MEMADD_SIZE_8BIT && reg <= 0xFF);
	assert(len == 1 && "a write is the register address then one byte");
	assert(timeout > 0);
	if (absent)
		return HAL_ERROR;	/* a NACKed transfer */
	if (bus_status != HAL_OK)
		return bus_status;
	writes++;
	dev_addr = address >> 1;
	if (log_len < (int)sizeof(log_reg)) {
		log_reg[log_len] = (uint8_t)reg;
		log_val[log_len++] = data[0];
	}

	switch (reg) {
	case QMC5883P_CTRL1:
		write_ctrl1(data[0]);
		break;
	case QMC5883P_CTRL2:
		write_ctrl2(data[0]);
		break;
	case QMC5883P_AXIS_SIGN:
		regs[reg] = data[0];
		break;
	default:
		assert(!"the driver writes only CTRL1, CTRL2 and 29H");
	}
	return HAL_OK;
}

/*
 * One measurement of field + extra (gauss) at the RNG currently in CTRL2,
 * latched into the data registers: saturating at the int16 limits (§9.2.1),
 * OVFL past 30000 counts (§9.2.2), DRDY set.
 */
static void latch(const float extra[3])
{
	int rng = (regs[QMC5883P_CTRL2] >> QMC5883P_RNG_SHIFT) & 0x03;

	for (int i = 0; i < 3; i++) {
		float c = lroundf((field_g[i] + extra[i]) *
				  qmc5883p_lsb_per_gauss(rng));

		if (c > QMC5883P_OVFL_COUNTS || c < -QMC5883P_OVFL_COUNTS)
			regs[QMC5883P_STATUS] |= QMC5883P_STATUS_OVFL;
		out[i] = (int16_t)(c > 32767.0f ? 32767 :
				   c < -32768.0f ? -32768 : c);
	}
	regs[QMC5883P_STATUS] |= QMC5883P_STATUS_DRDY;
}

/* A measurement right now, as if the ODR timer had just fired. */
static void measure(void)
{
	static const float none[3];

	latch(none);
}

/*
 * The stimulus lands once 5 ms have passed: X down, Y and Z up, so §7.3's
 * signs turn it into positive deltas. Then §6.2.3's drop to suspend.
 */
static void settle_self_test(void)
{
	float st[3] = { -stimulus_g[0], stimulus_g[1], stimulus_g[2] };

	if (!st_pending || elapsed_ms - st_started_ms < 5)
		return;
	latch(st);
	regs[QMC5883P_CTRL2] &= (uint8_t)~QMC5883P_SELF_TEST;
	regs[QMC5883P_CTRL1] &= (uint8_t)~0x03;
	st_pending = false;
}

static uint8_t read_status(void)
{
	uint8_t s;

	status_reads++;
	if (mode() != QMC5883P_MODE_SUSPEND && drdy_countdown >= 0) {
		if (drdy_countdown == 0)
			measure();
		drdy_countdown--;
	}
	s = regs[QMC5883P_STATUS];
	regs[QMC5883P_STATUS] = 0;	/* §9.2.2: both flags clear on read */
	return s;
}

/* §8.2.4: register address, repeated start, then the bytes back. */
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t address,
				   uint16_t reg, uint16_t reg_size,
				   uint8_t *rx, uint16_t rx_len,
				   uint32_t timeout)
{
	assert(hi2c == &bus);
	assert(!(address & 1) && "the HAL takes the 7-bit address << 1");
	assert(reg_size == I2C_MEMADD_SIZE_8BIT && reg <= 0xFF);
	assert(timeout > 0);
	if (absent)
		return HAL_ERROR;
	if (bus_status != HAL_OK)
		return bus_status;
	reads++;
	dev_addr = address >> 1;
	settle_self_test();

	if (reg == QMC5883P_CHIP_ID) {
		assert(rx_len == 1);
		rx[0] = chip_id;
		return HAL_OK;
	}
	if (reg == QMC5883P_STATUS) {
		assert(rx_len == 1);
		rx[0] = read_status();
		return HAL_OK;
	}
	if (reg == QMC5883P_DATA_X_LSB) {
		assert(rx_len == QMC5883P_DATA_LEN && "all three axes in one burst");
		data_reads++;
		for (int i = 0; i < 3; i++) {
			rx[2 * i] = (uint8_t)(out[i] & 0xFF);
			rx[2 * i + 1] = (uint8_t)((uint16_t)out[i] >> 8);
		}
		return HAL_OK;
	}
	assert((size_t)reg + rx_len <= sizeof(regs));
	memcpy(rx, &regs[reg], rx_len);
	return HAL_OK;
}

/* HAL_Delay(n): n + 1 ticks, of which only n whole milliseconds are sure. */
void HAL_Delay(uint32_t ms)
{
	stub_tick += ms + 1;
	elapsed_ms += (long)ms;
}

/* Counts the driver's calloc() and free() calls; see test/Makefile. */
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}
void __real_free(void *p);
void __wrap_free(void *p);
void __wrap_free(void *p)
{
	frees++;
	__real_free(p);
}

static struct compass_dev open_ok(const struct qmc5883p_config *cfg)
{
	struct compass_dev compass = { 0 };

	chip_power_on();
	assert(qmc5883p_probe(&compass, &bus, cfg) == 0);
	assert(compass.ops && compass.priv);
	return compass;
}

/* The writes since power-on, as reg/value pairs, must be exactly these. */
static void expect_writes(const uint8_t *pairs, int n)
{
	assert(log_len == n);
	for (int i = 0; i < n; i++)
		assert(log_reg[i] == pairs[2 * i] &&
		       log_val[i] == pairs[2 * i + 1]);
}

/* ------------------------------------------------------------- tests ----- */

static void test_probe_defaults(void)
{
	/*
	 * Soft reset (§7.6), 29H (§7.1 step 1), CTRL2 (range 30 G, set/reset
	 * on), then CTRL1 through suspend into normal at 10 Hz.
	 */
	static const uint8_t seq[] = {
		0x0B, 0x80, 0x29, 0x06, 0x0B, 0x00, 0x0A, 0x00, 0x0A, 0x01,
	};
	struct compass_dev compass = open_ok(NULL);

	assert(!strcmp(compass.name, "qmc5883p"));
	assert(dev_addr == 0x2C && probed_addr == 0x2C);
	assert(resets == 1);
	expect_writes(seq, (int)(sizeof(seq) / 2));
	assert(mode_violations == 0 && sign_missing == 0);
	assert(strstr(stub_log_last, "compass: qmc5883p: at 0x2c"));

	/* A second probe on a bound device touches nothing. */
	writes = reads = allocs = 0;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -EBUSY);
	assert(writes == 0 && reads == 0 && allocs == 0);

	qmc5883p_remove(&compass);
	assert(mode() == QMC5883P_MODE_SUSPEND && "parked in suspend");
	assert(frees == 1 && !compass.ops && !compass.priv);
	qmc5883p_remove(&compass);	/* unprobed: a no-op */
	assert(frees == 1);
}

static void test_probe_matches_section_7(void)
{
	/* §7.2 continuous setup: 29H=06, 0BH=08, 0AH=C3. */
	static const uint8_t seq72[] = {
		0x0B, 0x80, 0x29, 0x06, 0x0B, 0x08, 0x0A, 0x00, 0x0A, 0xC3,
	};
	/* §7.1 normal setup: 29H=06, 0BH=08, 0AH=CD. */
	static const uint8_t seq71[] = {
		0x0B, 0x80, 0x29, 0x06, 0x0B, 0x08, 0x0A, 0x00, 0x0A, 0xCD,
	};
	struct qmc5883p_config c = {
		.range = QMC5883P_RANGE_800UT,
		.odr = QMC5883P_ODR_10HZ,
		.osr1 = QMC5883P_OSR1_8,
		.osr2 = QMC5883P_OSR2_8,
		.continuous = true,
	};
	struct compass_dev compass = open_ok(&c);

	expect_writes(seq72, (int)(sizeof(seq72) / 2));
	qmc5883p_remove(&compass);

	c.continuous = false;
	c.odr = QMC5883P_ODR_200HZ;
	compass = open_ok(&c);
	expect_writes(seq71, (int)(sizeof(seq71) / 2));
	qmc5883p_remove(&compass);
}

static void test_not_a_qmc5883p(void)
{
	struct compass_dev compass = { 0 };

	/* Nothing at 0x2C: the GY-271 may carry a QMC5883L instead. */
	chip_power_on();
	absent = true;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -ENXIO);
	assert(probed_addr == QMC5883P_ADDR);
	assert(allocs == 0 && writes == 0 && reads == 0 &&
	       "decided before touching anything");
	assert(!compass.ops && !compass.priv);
	/* Debug level only: test/Makefile turns CONFIG_COMPASS_DEBUG on. */
	assert(strstr(stub_log_last, "compass: qmc5883p: nothing answered"));

	/* Something answers with the wrong ID: refused before any write. */
	chip_power_on();
	chip_id = 0xFF;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -ENODEV);
	assert(strstr(stub_log_last, "compass: qmc5883p: CHIPID"));
	assert(writes == 0 && allocs == 1 && frees == 1);
	assert(!compass.ops && !compass.priv);
}

/* HAL_StatusTypeDef -> errno, through probe, read and the self-test. */
static void test_errno(void)
{
	struct compass_dev compass = { 0 };
	struct compass_sample s;
	int16_t delta[3];

	/* A bus that is busy or hangs during the presence check is not absence. */
	chip_power_on();
	probe_status = HAL_BUSY;	/* also: a handle never initialised */
	assert(qmc5883p_probe(&compass, &bus, NULL) == -EAGAIN);
	assert(allocs == 0 && writes == 0 && reads == 0);
	assert(strstr(stub_log_last, "presence check at 0x2c failed (-"));
	chip_power_on();
	probe_status = HAL_TIMEOUT;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -ETIMEDOUT);
	assert(allocs == 0);

	/* A NACK in the middle of a transfer is an I/O error, not absence. */
	chip_power_on();
	bus_status = HAL_ERROR;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -EIO);
	assert(allocs == 1 && frees == 1 && !compass.ops && !compass.priv);
	/* Not -EBUSY: from a probe that means "already bound". */
	chip_power_on();
	bus_status = HAL_BUSY;
	assert(qmc5883p_probe(&compass, &bus, NULL) == -EAGAIN);
	assert(allocs == 1 && frees == 1);

	compass = open_ok(NULL);
	bus_status = HAL_TIMEOUT;
	assert(compass_read(&compass, &s) == -ETIMEDOUT);
	assert(qmc5883p_self_test(&compass, delta) == -ETIMEDOUT);
	bus_status = HAL_BUSY;
	assert(compass_read(&compass, &s) == -EAGAIN);
	bus_status = HAL_OK;
	qmc5883p_remove(&compass);
	assert(frees == 1);
}

static void test_bad_config(void)
{
	struct qmc5883p_config c = { 0 };
	struct compass_dev compass = { 0 };

	/* Refused before anything touches the bus, and nothing allocated. */
	chip_power_on();
	c.range = (enum qmc5883p_range)4;
	assert(qmc5883p_probe(&compass, &bus, &c) == -EINVAL);
	c.range = 0;
	c.odr = (enum qmc5883p_odr)4;
	assert(qmc5883p_probe(&compass, &bus, &c) == -EINVAL);
	c.odr = 0;
	c.osr1 = (enum qmc5883p_osr1)4;
	assert(qmc5883p_probe(&compass, &bus, &c) == -EINVAL);
	c.osr1 = 0;
	c.osr2 = (enum qmc5883p_osr2)-1;
	assert(qmc5883p_probe(&compass, &bus, &c) == -EINVAL);
	c.osr2 = 0;
	assert(qmc5883p_probe(NULL, &bus, &c) == -EINVAL);
	assert(qmc5883p_probe(&compass, NULL, &c) == -EINVAL);
	assert(writes == 0 && reads == 0 && probed_addr == 0 && allocs == 0);
	assert(!compass.ops && !compass.priv);
}

static void test_read(void)
{
	struct qmc5883p_config c = { .range = QMC5883P_RANGE_200UT };
	struct compass_dev compass = open_ok(&c);
	struct compass_sample s;
	bool ready;

	field_g[0] = 0.01f;	/* 150 counts at 2 G */
	field_g[1] = -0.02f;
	field_g[2] = 2.0f;	/* 30000: full scale, not yet OVFL */
	measure();
	status_reads = data_reads = 0;
	stub_tick = 4242;
	assert(compass_read(&compass, &s) == 0);
	assert(status_reads == 1 && data_reads == 1 && "§7.5: status, data");
	assert(fabsf(s.field[0] - 1.0f) < 1e-4f);	/* 150 counts at 2 G */
	assert(fabsf(s.field[1] + 2.0f) < 1e-4f);
	assert(fabsf(s.field[2] - 200.0f) < 1e-3f);	/* full scale */
	assert(s.t_us == 4242000);

	/* OVFL refuses the sample and leaves s alone. */
	regs[QMC5883P_STATUS] = QMC5883P_STATUS_OVFL;
	s.field[0] = 99.0f;
	assert(compass_read(&compass, &s) == -ERANGE);
	assert(s.field[0] == 99.0f);
	assert(compass_read(&compass, &s) == 0 && "reported once");

	/*
	 * A data_ready() poll reads STATUS, which clears OVFL on the chip
	 * (§9.2.2). The driver must still report it on the next read.
	 */
	regs[QMC5883P_STATUS] = QMC5883P_STATUS_OVFL | QMC5883P_STATUS_DRDY;
	assert(qmc5883p_data_ready(&compass, &ready) == 0 && ready);
	assert(regs[QMC5883P_STATUS] == 0);
	assert(compass_read(&compass, &s) == -ERANGE);
	assert(compass_read(&compass, &s) == 0);

	/* Wider range: fewer counts, the same microtesla. */
	assert(qmc5883p_set_range(&compass, QMC5883P_RANGE_3000UT) == 0);
	assert(regs[QMC5883P_CTRL2] == 0x00);
	assert(mode() == QMC5883P_MODE_NORMAL && "range change keeps the mode");
	measure();
	assert(out[0] == 10 && "0.01 G at 1000 LSB/G");
	assert(compass_read(&compass, &s) == 0);
	assert(fabsf(s.field[0] - 1.0f) < 1e-3f);
	assert(qmc5883p_set_range(&compass, (enum qmc5883p_range)4) == -EINVAL);

	/* A NACK mid-read is the read's error. */
	absent = true;
	assert(compass_read(&compass, &s) == -EIO);
	absent = false;
	qmc5883p_remove(&compass);
}

static void test_data_ready_and_raw(void)
{
	struct compass_dev compass = open_ok(NULL);
	int16_t raw[3];
	bool ready = true;

	/* drdy_delay 1: the first poll after entering normal mode sees none. */
	assert(qmc5883p_data_ready(&compass, &ready) == 0 && !ready);
	assert(qmc5883p_data_ready(&compass, &ready) == 0 && ready);
	assert(qmc5883p_data_ready(&compass, &ready) == 0 && !ready &&
	       "cleared by the read before");

	/* §9.2.1: outputs saturate at the int16 limits. */
	field_g[0] = -40.0f;
	field_g[1] = 40.0f;
	field_g[2] = -0.001f;
	measure();
	assert(qmc5883p_read_raw(&compass, raw) == 0);
	assert(raw[0] == -32768 && raw[1] == 32767 && raw[2] == -1);
	qmc5883p_remove(&compass);
}

static void test_standby(void)
{
	struct qmc5883p_config c = { .continuous = true };
	struct compass_dev compass = open_ok(&c);

	assert(qmc5883p_standby(&compass, true) == 0);
	assert(mode() == QMC5883P_MODE_SUSPEND);
	assert(qmc5883p_standby(&compass, false) == 0);
	assert(mode() == QMC5883P_MODE_CONTINUOUS);
	/* Waking while already awake still goes through suspend. */
	assert(qmc5883p_standby(&compass, false) == 0);
	assert(mode() == QMC5883P_MODE_CONTINUOUS);
	assert(mode_violations == 0);
	qmc5883p_remove(&compass);
}

static void test_self_test(void)
{
	struct qmc5883p_config c = { .range = QMC5883P_RANGE_800UT,
				     .odr = QMC5883P_ODR_50HZ };
	struct compass_dev compass = open_ok(&c);
	struct compass_sample s;
	int16_t delta[3] = { 0 };
	bool ready = true;

	/*
	 * Probed at +/-8 G, with a measurement at that range already waiting:
	 * DRDY set, data at 3750 LSB/G. Both self-test readings must come from
	 * 30 G, or the delta compares two scales. Z sits close enough to 30 G
	 * that the stimulus overflows it.
	 */
	field_g[0] = 1.0f;
	field_g[1] = -2.0f;
	field_g[2] = 29.8f;
	measure();
	assert(regs[QMC5883P_STATUS] & QMC5883P_STATUS_DRDY);
	assert(qmc5883p_self_test(&compass, delta) == 0);
	/* §7.3's signs turn the fake's stimulus back into positive deltas. */
	assert(delta[0] == 300 && delta[1] == 400 && delta[2] == 500);
	assert(self_test_wrong_mode == 0 && "§6.2.3: continuous mode only");
	assert(elapsed_ms - st_started_ms >= 5 && "§7.3 step 6 waited out");
	assert(mode_violations == 0 && sign_missing == 0);

	/* Range and mode come back; §7.3's 0x40 had set RNG to 30 G. */
	assert(regs[QMC5883P_CTRL2] == qmc5883p_ctrl2(QMC5883P_RANGE_800UT));
	assert(regs[QMC5883P_CTRL1] ==
	       qmc5883p_ctrl1(0, 0, QMC5883P_ODR_50HZ, QMC5883P_MODE_NORMAL));

	/*
	 * The stimulated reading is still in the data registers. data_ready()
	 * must not claim it as new: the driver read STATUS after the restore.
	 */
	drdy_countdown = -1;
	assert(qmc5883p_data_ready(&compass, &ready) == 0 && !ready);

	/* The overflow the stimulus caused is not the caller's. */
	assert(compass_read(&compass, &s) == 0);
	qmc5883p_remove(&compass);
}

static void test_self_test_timeout(void)
{
	struct compass_dev compass = open_ok(NULL);
	int16_t delta[3] = { 7, 7, 7 };
	uint32_t start;

	/* No measurement ever arrives: -ETIMEDOUT, delta untouched, and the
	 * configured state restored anyway. */
	drdy_delay = -1;
	stub_tick = UINT32_MAX - 50;	/* the wait straddles the tick's wrap */
	start = stub_tick;
	assert(qmc5883p_self_test(&compass, delta) == -ETIMEDOUT);
	/* Gave up only once more than 100 ms had passed by HAL_GetTick(). */
	assert((uint32_t)(stub_tick - start) > 100);
	assert((uint32_t)(stub_tick - start) < 110);
	assert(delta[0] == 7 && delta[1] == 7 && delta[2] == 7);
	assert(strstr(stub_log_last, "self-test failed"));
	assert(regs[QMC5883P_CTRL2] == 0x00);
	assert(mode() == QMC5883P_MODE_NORMAL);
	assert(mode_violations == 0);
	qmc5883p_remove(&compass);
}

static int other_read(struct compass_dev *compass, struct compass_sample *s)
{
	(void)compass;
	(void)s;
	return 0;
}

static void test_bad_arguments(void)
{
	static const struct compass_ops other_ops = { .read = other_read };
	struct compass_dev other = { .name = "other", .ops = &other_ops };
	struct compass_dev compass;
	int16_t f[3];
	bool b;
	uint8_t v;

	/* Extras on a NULL device, or one another driver probed, are refused. */
	assert(qmc5883p_read_raw(NULL, f) == -EINVAL);
	assert(qmc5883p_read_raw(&other, f) == -EINVAL);
	assert(qmc5883p_data_ready(&other, &b) == -EINVAL);
	assert(qmc5883p_set_range(&other, QMC5883P_RANGE_200UT) == -EINVAL);
	assert(qmc5883p_standby(&other, true) == -EINVAL);
	assert(qmc5883p_self_test(&other, f) == -EINVAL);
	assert(qmc5883p_reg_read(&other, 0, &v, 1) == -EINVAL);
	assert(qmc5883p_reg_write(&other, 0, 0) == -EINVAL);
	qmc5883p_remove(&other);	/* not ours: left alone */
	assert(other.ops == &other_ops);

	compass = open_ok(NULL);
	assert(qmc5883p_read_raw(&compass, NULL) == -EINVAL);
	assert(qmc5883p_data_ready(&compass, NULL) == -EINVAL);
	assert(qmc5883p_self_test(&compass, NULL) == -EINVAL);
	assert(qmc5883p_reg_read(&compass, 0, &v, 0) == -EINVAL);
	assert(qmc5883p_reg_read(&compass, 0, NULL, 1) == -EINVAL);
	/* The HAL counts bytes in a uint16_t; more is refused, not truncated. */
	reads = 0;
	assert(qmc5883p_reg_read(&compass, 0, &v, (size_t)UINT16_MAX + 1) ==
	       -EINVAL);
	assert(reads == 0);
	assert(qmc5883p_reg_read(&compass, QMC5883P_CHIP_ID, &v, 1) == 0 &&
	       v == 0x80);
	assert(qmc5883p_reg_write(&compass, QMC5883P_AXIS_SIGN, 0x06) == 0);
	qmc5883p_remove(&compass);
}

int main(void)
{
	test_probe_defaults();
	test_probe_matches_section_7();
	test_not_a_qmc5883p();
	test_errno();
	test_bad_config();
	test_read();
	test_data_ready_and_raw();
	test_standby();
	test_self_test();
	test_self_test_timeout();
	test_bad_arguments();
	printf("all qmc5883p driver checks passed\n");
	return 0;
}
