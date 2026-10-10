/*
 * Host-side check of the QMC5883L driver against a fake chip behind a fake
 * HAL. Ported from esp-compass's test_qmc5883l.c, itself from
 * esp-qmc5883l-mag's test_driver.c.
 *
 * The interesting case is absence. This part's identity register reads 0xFF,
 * which is exactly what an I2C bus with a pull-up and no device reads, so a
 * driver that trusts the identity alone reports success on an empty bus. The
 * "no device" tests below are the point of this file.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass.h"
#include "qmc5883l.h"
#include "qmc5883l_math.h"
#include "qmc5883l_regs.h"

static I2C_HandleTypeDef bus;
static uint8_t regs[256];
static uint16_t dev_addr, probed_addr;	/* 7-bit */
static bool floating_bus;	/* answers, but every read gives 0xFF */
static bool absent;		/* nothing ACKs at all */
static HAL_StatusTypeDef bus_status;	/* every transfer returns this if set */
static HAL_StatusTypeDef probe_status;	/* IsDeviceReady on a present chip */
static int reset_count, longest_read, writes, reads, allocs, frees;
static long slept_ms;

static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	regs[QMC5883L_CHIP_ID] = QMC5883L_CHIP_ID_VALUE;
	floating_bus = absent = false;
	bus_status = probe_status = HAL_OK;
	dev_addr = probed_addr = 0;
	reset_count = longest_read = writes = reads = allocs = frees = 0;
	slept_ms = 0;
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

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c,
				    uint16_t address, uint16_t reg,
				    uint16_t reg_size, uint8_t *data,
				    uint16_t len, uint32_t timeout)
{
	assert(hi2c == &bus);
	assert(!(address & 1) && "the HAL takes the 7-bit address << 1");
	assert(reg_size == I2C_MEMADD_SIZE_8BIT && reg <= 0xFF);
	assert(len == 1 && "a register write is one value");
	assert(timeout > 0);
	if (absent)
		return HAL_ERROR;	/* a NACKed transfer */
	if (bus_status != HAL_OK)
		return bus_status;
	writes++;
	dev_addr = address >> 1;
	if (floating_bus)
		return HAL_OK;		/* the write goes nowhere */
	if (reg == QMC5883L_CTRL2 && (data[0] & QMC5883L_SOFT_RST)) {
		uint8_t id = regs[QMC5883L_CHIP_ID];

		memset(regs, 0, sizeof(regs));
		regs[QMC5883L_CHIP_ID] = id;
		reset_count++;
		return HAL_OK;
	}
	regs[reg] = data[0];
	return HAL_OK;
}

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
	if ((int)rx_len > longest_read)
		longest_read = (int)rx_len;
	for (size_t i = 0; i < rx_len; i++)
		rx[i] = floating_bus ? 0xFF : regs[(reg + i) & 0xFF];
	return HAL_OK;
}

/* HAL_Delay(n): n + 1 ticks, of which only n whole milliseconds are sure. */
void HAL_Delay(uint32_t ms)
{
	stub_tick += ms + 1;
	slept_ms += (long)ms;
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

static struct qmc5883l_config base_cfg(void)
{
	struct qmc5883l_config c = {
		.range = QMC5883L_RANGE_200UT,
		.odr = QMC5883L_ODR_100HZ,
		.osr = QMC5883L_OSR_512,
	};
	return c;
}

static struct compass_dev open_ok(void)
{
	struct qmc5883l_config c = base_cfg();
	struct compass_dev compass = { 0 };

	chip_reset();
	assert(qmc5883l_probe(&compass, &bus, &c) == 0);
	assert(compass.ops && compass.priv);
	return compass;
}

static void put(int reg, int16_t v)	/* little endian, low byte first */
{
	regs[reg] = (uint8_t)(v & 0xFF);
	regs[reg + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* ---------------------------------------------------- absence detection -- */

static void test_empty_bus_is_not_a_device(void)
{
	struct qmc5883l_config c = base_cfg();
	struct compass_dev compass = { 0 };

	/* Nothing ACKs: the plain "not on this bus" answer, quietly. */
	chip_reset();
	absent = true;
	assert(qmc5883l_probe(&compass, &bus, &c) == -ENXIO);
	assert(probed_addr == QMC5883L_ADDR);
	assert(allocs == 0 && writes == 0 && reads == 0 &&
	       "decided before touching anything");
	assert(!compass.ops && !compass.priv);
	/* Debug level only: test/Makefile turns CONFIG_COMPASS_DEBUG on. */
	assert(strstr(stub_log_last, "compass: qmc5883l: nothing answered"));

	/* Every read returns 0xFF, which matches the identity value exactly.
	 * Trusting the ID alone would report success here. */
	chip_reset();
	floating_bus = true;
	assert(qmc5883l_probe(&compass, &bus, &c) == -ENODEV);
	assert(strstr(stub_log_last, "floating bus"));
	assert(allocs == 1 && frees == 1 && !compass.ops && !compass.priv);

	/* A device that answers with the wrong identity is also refused. The
	 * log points at the two parts it is most likely to be. */
	chip_reset();
	regs[QMC5883L_CHIP_ID] = 0x48;
	assert(qmc5883l_probe(&compass, &bus, &c) == -ENODEV);
	assert(strstr(stub_log_last, "compass: qmc5883l: "));
	assert(strstr(stub_log_last, "0x1E") && strstr(stub_log_last, "0x2C"));

	chip_reset();
	regs[QMC5883L_CHIP_ID] = 0x00;
	assert(qmc5883l_probe(&compass, &bus, &c) == -ENODEV);
	assert(allocs == frees);
}

/* HAL_StatusTypeDef -> errno, through probe and read. */
static void test_errno(void)
{
	struct qmc5883l_config c = base_cfg();
	struct compass_dev compass = { 0 };
	struct compass_sample s;

	/* A bus that is busy or hangs during the presence check is not absence. */
	chip_reset();
	probe_status = HAL_BUSY;	/* also: a handle never initialised */
	assert(qmc5883l_probe(&compass, &bus, &c) == -EAGAIN);
	assert(allocs == 0 && writes == 0 && reads == 0);
	assert(strstr(stub_log_last, "presence check at 0x0d failed (-"));
	chip_reset();
	probe_status = HAL_TIMEOUT;
	assert(qmc5883l_probe(&compass, &bus, &c) == -ETIMEDOUT);
	assert(allocs == 0);

	/* A NACK in the middle of a transfer is an I/O error, not absence. */
	chip_reset();
	bus_status = HAL_ERROR;
	assert(qmc5883l_probe(&compass, &bus, &c) == -EIO);
	assert(allocs == 1 && frees == 1 && !compass.ops && !compass.priv);
	/* Not -EBUSY: from a probe that means "already bound". */
	chip_reset();
	bus_status = HAL_BUSY;
	assert(qmc5883l_probe(&compass, &bus, &c) == -EAGAIN);
	assert(allocs == 1 && frees == 1);

	compass = open_ok();
	bus_status = HAL_TIMEOUT;
	assert(compass_read(&compass, &s) == -ETIMEDOUT);
	bus_status = HAL_BUSY;
	assert(compass_read(&compass, &s) == -EAGAIN);
	bus_status = HAL_OK;
	qmc5883l_remove(&compass);
	assert(frees == 1);
}

static void test_probe(void)
{
	struct compass_dev compass = open_ok();

	assert(!strcmp(compass.name, "qmc5883l"));
	assert(dev_addr == QMC5883L_ADDR);
	assert(reset_count == 1);
	assert(slept_ms >= QMC5883L_RESET_WAIT_MS && "waited out the reset");
	/* SET/RESET must be left at the recommended value; the part misbehaves
	 * without it, and probe also uses it to prove the device stores writes. */
	assert(regs[QMC5883L_SET_RESET] == QMC5883L_SET_RESET_VAL);
	/* INT_ENB set means the interrupt pin is disabled. */
	assert(regs[QMC5883L_CTRL2] & QMC5883L_INT_ENB);
	/* Continuous mode, with the configured range/rate/oversampling. */
	assert((regs[QMC5883L_CTRL1] & 0x03) == QMC5883L_MODE_CONT);
	assert(((regs[QMC5883L_CTRL1] >> QMC5883L_ODR_SHIFT) & 0x3) ==
	       QMC5883L_ODR_100HZ);
	assert(((regs[QMC5883L_CTRL1] >> QMC5883L_RNG_SHIFT) & 0x3) ==
	       QMC5883L_RANGE_200UT);
	assert(((regs[QMC5883L_CTRL1] >> QMC5883L_OSR_SHIFT) & 0x3) ==
	       QMC5883L_OSR_512);
	assert(strstr(stub_log_last, "compass: qmc5883l: at 0x0d"));

	/* A second probe on a bound device touches nothing. */
	writes = reads = allocs = 0;
	assert(qmc5883l_probe(&compass, &bus, NULL) == -EBUSY);
	assert(writes == 0 && reads == 0 && allocs == 0);

	qmc5883l_remove(&compass);
	/* Teardown parks it in standby rather than leaving it converting. */
	assert((regs[QMC5883L_CTRL1] & 0x03) == QMC5883L_MODE_STANDBY);
	assert(frees == 1 && !compass.ops && !compass.priv);
	qmc5883l_remove(&compass);	/* unprobed: a no-op */
	assert(frees == 1);

	/* NULL config is the all-zero one. */
	chip_reset();
	assert(qmc5883l_probe(&compass, &bus, NULL) == 0);
	assert(regs[QMC5883L_CTRL1] == QMC5883L_MODE_CONT);
	qmc5883l_remove(&compass);
}

static void test_bad_config(void)
{
	struct qmc5883l_config c;
	struct compass_dev compass = { 0 };

	/* Refused before anything touches the bus, and nothing allocated. */
	chip_reset();
	c = base_cfg();
	c.range = (enum qmc5883l_range)2;
	assert(qmc5883l_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	c.odr = (enum qmc5883l_odr)4;
	assert(qmc5883l_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	c.osr = (enum qmc5883l_osr)4;
	assert(qmc5883l_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	assert(qmc5883l_probe(NULL, &bus, &c) == -EINVAL);
	assert(qmc5883l_probe(&compass, NULL, &c) == -EINVAL);
	assert(writes == 0 && reads == 0 && probed_addr == 0 && allocs == 0);
	assert(!compass.ops && !compass.priv);
}

static void test_read(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s = { .t_us = 0 };
	int16_t raw[3];

	put(0x00, 120);
	put(0x02, -240);
	put(0x04, 24000);
	regs[QMC5883L_STATUS] = QMC5883L_STATUS_DRDY;

	longest_read = 0;
	assert(qmc5883l_read_raw(&compass, raw) == 0);
	assert(longest_read == QMC5883L_DATA_LEN && "one burst of six bytes");
	assert(raw[0] == 120 && raw[1] == -240 && raw[2] == 24000);

	stub_tick = 777;
	assert(compass_read(&compass, &s) == 0);
	assert(fabs(s.field[0] - 1.0f) < 1e-4);	/* 120 counts is 1 uT at 2 G */
	assert(fabs(s.field[1] + 2.0f) < 1e-4);
	assert(fabs(s.field[2] - 200.0f) < 1e-3);	/* full scale */
	assert(s.t_us == 777000);

	/* Overflow has to reach the caller: a clipped reading looks normal. */
	regs[QMC5883L_STATUS] = QMC5883L_STATUS_DRDY | QMC5883L_STATUS_OVL;
	s.field[0] = 99.0f;
	assert(compass_read(&compass, &s) == -ERANGE);
	assert(s.field[0] == 99.0f && "a refused sample is not written");
	regs[QMC5883L_STATUS] = QMC5883L_STATUS_DRDY;

	/* Same counts, wider range, four times the field. */
	assert(qmc5883l_set_range(&compass, QMC5883L_RANGE_800UT) == 0);
	assert(compass_read(&compass, &s) == 0);
	assert(fabs(s.field[0] - 4.0f) < 1e-4);
	/* Changing range must not knock it out of continuous mode. */
	assert((regs[QMC5883L_CTRL1] & 0x03) == QMC5883L_MODE_CONT);

	assert(qmc5883l_set_range(&compass, (enum qmc5883l_range)2) == -EINVAL);

	/* A NACK mid-read is the read's error. */
	absent = true;
	assert(compass_read(&compass, &s) == -EIO);
	absent = false;
	qmc5883l_remove(&compass);
}

static void test_status_and_temp(void)
{
	struct compass_dev compass = open_ok();
	bool ready = true;
	float t = 99.0f;

	regs[QMC5883L_STATUS] = 0;
	assert(qmc5883l_data_ready(&compass, &ready) == 0 && !ready);
	regs[QMC5883L_STATUS] = QMC5883L_STATUS_DRDY;
	assert(qmc5883l_data_ready(&compass, &ready) == 0 && ready);
	/* Overflow on its own is not new data. */
	regs[QMC5883L_STATUS] = QMC5883L_STATUS_OVL;
	assert(qmc5883l_data_ready(&compass, &ready) == 0 && !ready);

	put(QMC5883L_TEMP_LSB, 2500);
	assert(qmc5883l_temperature(&compass, &t) == 0);
	assert(fabs(t - 25.0f) < 1e-4);
	put(QMC5883L_TEMP_LSB, -1000);
	assert(qmc5883l_temperature(&compass, &t) == 0);
	assert(fabs(t + 10.0f) < 1e-4);

	qmc5883l_remove(&compass);
}

static void test_standby(void)
{
	struct compass_dev compass = open_ok();

	assert(qmc5883l_standby(&compass, true) == 0);
	assert((regs[QMC5883L_CTRL1] & 0x03) == QMC5883L_MODE_STANDBY);
	/* Standby must not lose the range and rate. */
	assert(((regs[QMC5883L_CTRL1] >> QMC5883L_ODR_SHIFT) & 0x3) ==
	       QMC5883L_ODR_100HZ);
	assert(qmc5883l_standby(&compass, false) == 0);
	assert((regs[QMC5883L_CTRL1] & 0x03) == QMC5883L_MODE_CONT);
	qmc5883l_remove(&compass);
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
	float t;
	uint8_t v;

	/* Extras on a NULL device, or one another driver probed, are refused. */
	assert(qmc5883l_read_raw(NULL, f) == -EINVAL);
	assert(qmc5883l_read_raw(&other, f) == -EINVAL);
	assert(qmc5883l_data_ready(&other, &b) == -EINVAL);
	assert(qmc5883l_temperature(&other, &t) == -EINVAL);
	assert(qmc5883l_set_range(&other, QMC5883L_RANGE_200UT) == -EINVAL);
	assert(qmc5883l_standby(&other, true) == -EINVAL);
	assert(qmc5883l_reg_read(&other, 0, &v, 1) == -EINVAL);
	assert(qmc5883l_reg_write(&other, 0, 0) == -EINVAL);
	qmc5883l_remove(&other);	/* not ours: left alone */
	assert(other.ops == &other_ops);

	compass = open_ok();
	assert(qmc5883l_read_raw(&compass, NULL) == -EINVAL);
	assert(qmc5883l_data_ready(&compass, NULL) == -EINVAL);
	assert(qmc5883l_temperature(&compass, NULL) == -EINVAL);
	assert(qmc5883l_reg_read(&compass, 0, &v, 0) == -EINVAL);
	assert(qmc5883l_reg_read(&compass, 0, NULL, 1) == -EINVAL);
	/* The HAL counts bytes in a uint16_t; more is refused, not truncated. */
	reads = 0;
	assert(qmc5883l_reg_read(&compass, 0, &v, (size_t)UINT16_MAX + 1) ==
	       -EINVAL);
	assert(reads == 0);
	assert(qmc5883l_reg_write(&compass, 0x20, 0x5A) == 0);
	assert(qmc5883l_reg_read(&compass, 0x20, &v, 1) == 0 && v == 0x5A);
	qmc5883l_remove(&compass);
}

int main(void)
{
	test_empty_bus_is_not_a_device();
	test_errno();
	test_probe();
	test_bad_config();
	test_read();
	test_status_and_temp();
	test_standby();
	test_bad_arguments();
	printf("all qmc5883l driver checks passed\n");
	return 0;
}
