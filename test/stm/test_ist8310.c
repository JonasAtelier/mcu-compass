/*
 * Host-side check of the IST8310 driver against a fake chip behind a fake
 * HAL.
 *
 * The fake follows the datasheet's single-measurement cycle (§3.1.2): CNTL1
 * = 0001 starts a measurement, which lands MEASURE_MS later, sets DRDY and
 * puts CNTL1 back to 0000. Reading a data register clears DRDY (§3.4). So
 * the tests can check that compass_read() never waits, starts the next
 * measurement each time it takes one, and recovers from a lost trigger.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass.h"
#include "ist8310.h"
#include "ist8310_math.h"
#include "ist8310_regs.h"

#define MEASURE_MS	6	/* §3.1.1, with 16x averaging */

static I2C_HandleTypeDef bus;
static uint8_t regs[256];
static int16_t field[3];	/* what the next measurement will produce */
static uint16_t dev_addr, probed_addr;	/* 7-bit */
static bool absent;
static bool lose_trigger;	/* the next CNTL1 write is ACKed but ignored */
static bool fail_trigger;	/* CNTL1 writes fail on the bus */
static HAL_StatusTypeDef bus_status;
static HAL_StatusTypeDef probe_status;
static int reset_count, triggers, writes, reads, allocs, frees;
static bool converting;
static uint32_t started;
static long slept_ms;

/* §6.4: every register at its default, nothing measuring. */
static void chip_reset(void)
{
	uint8_t id = regs[IST8310_WAI];

	memset(regs, 0, sizeof(regs));
	regs[IST8310_WAI] = id;
	regs[IST8310_CNTL2] = IST8310_CNTL2_DREN | IST8310_CNTL2_DRP;
	converting = false;
}

static void chip_power_on(void)
{
	regs[IST8310_WAI] = IST8310_WAI_VALUE;
	chip_reset();
	memset(field, 0, sizeof(field));
	absent = lose_trigger = fail_trigger = false;
	bus_status = probe_status = HAL_OK;
	dev_addr = probed_addr = 0;
	reset_count = triggers = writes = reads = allocs = frees = 0;
	slept_ms = 0;
	stub_tick = 1000;
}

static void put(int reg, int16_t v)	/* little endian, low byte first */
{
	regs[reg] = (uint8_t)(v & 0xFF);
	regs[reg + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* Lands a measurement that has had its time. */
static void chip_run(void)
{
	if (!converting || (uint32_t)(stub_tick - started) < MEASURE_MS)
		return;
	converting = false;
	for (int i = 0; i < 3; i++)
		put(IST8310_DATA_X_LSB + 2 * i, field[i]);
	regs[IST8310_STAT1] |= IST8310_STAT1_DRDY;
	regs[IST8310_CNTL1] = IST8310_MODE_STANDBY;
}

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
		return HAL_ERROR;
	if (bus_status != HAL_OK)
		return bus_status;
	chip_run();
	writes++;
	dev_addr = address >> 1;
	if (reg == IST8310_CNTL2 && (data[0] & IST8310_CNTL2_SRST)) {
		chip_reset();		/* §6.4.7: the POR routine */
		reset_count++;
		return HAL_OK;
	}
	if (reg == IST8310_CNTL1 && data[0] == IST8310_MODE_SINGLE) {
		if (fail_trigger)
			return HAL_ERROR;
		if (lose_trigger) {
			lose_trigger = false;
			return HAL_OK;
		}
		assert(!converting && "triggered while measuring");
		triggers++;
		converting = true;
		started = stub_tick;
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
	chip_run();
	reads++;
	dev_addr = address >> 1;
	for (size_t i = 0; i < rx_len; i++)
		rx[i] = regs[(reg + i) & 0xFF];
	/* §3.4: reading the data clears DRDY and DOR. */
	if (reg + rx_len > IST8310_DATA_X_LSB &&
	    reg < IST8310_DATA_X_LSB + IST8310_DATA_LEN)
		regs[IST8310_STAT1] = 0;
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

static struct ist8310_config base_cfg(void)
{
	struct ist8310_config c = {
		.address = IST8310_ADDR_LL,
		.avg_xz = IST8310_AVG_16,
		.avg_y = IST8310_AVG_16,
	};
	return c;
}

static struct compass_dev open_ok(void)
{
	struct ist8310_config c = base_cfg();
	struct compass_dev compass = { 0 };

	chip_power_on();
	assert(ist8310_probe(&compass, &bus, &c) == 0);
	assert(compass.ops && compass.priv);
	return compass;
}

/* ------------------------------------------------------------- tests ----- */

static void test_probe(void)
{
	struct compass_dev compass = open_ok();

	assert(!strcmp(compass.name, "ist8310"));
	assert(probed_addr == IST8310_ADDR_LL && dev_addr == IST8310_ADDR_LL);
	assert(reset_count == 1);
	assert(slept_ms >= IST8310_RESET_WAIT_MS && "waited out the reset");
	/* §3.1.1's two initial settings. */
	assert(regs[IST8310_AVGCNTL] == 0x24);
	assert(regs[IST8310_PDCNTL] == IST8310_PDCNTL_NORMAL);
	/* DREN left on, or STAT1's DRDY never sets (§6.4.3). */
	assert(regs[IST8310_CNTL2] & IST8310_CNTL2_DREN);
	assert(triggers == 1 && converting && "first measurement started");
	assert(strstr(stub_log_last, "compass: ist8310: at 0x0c"));

	/* A second probe on a bound device touches nothing. */
	writes = reads = allocs = 0;
	assert(ist8310_probe(&compass, &bus, NULL) == -EBUSY);
	assert(writes == 0 && reads == 0 && allocs == 0);

	ist8310_remove(&compass);
	assert(regs[IST8310_CNTL1] == IST8310_MODE_STANDBY);
	assert(frees == 1 && !compass.ops && !compass.priv);
	ist8310_remove(&compass);	/* unprobed: a no-op */
	assert(frees == 1);

	/* NULL config: 0x0E, the floating-pins address, and no averaging. */
	chip_power_on();
	assert(ist8310_probe(&compass, &bus, NULL) == 0);
	assert(dev_addr == IST8310_ADDR_HL && regs[IST8310_AVGCNTL] == 0x00);
	ist8310_remove(&compass);
}

static void test_absent_and_wrong_chip(void)
{
	struct ist8310_config c = base_cfg();
	struct compass_dev compass = { 0 };

	chip_power_on();
	absent = true;
	assert(ist8310_probe(&compass, &bus, &c) == -ENXIO);
	assert(probed_addr == IST8310_ADDR_LL);
	assert(allocs == 0 && writes == 0 && reads == 0);
	assert(!compass.ops && !compass.priv);
	assert(strstr(stub_log_last, "compass: ist8310: nothing answered"));

	/* Something else answers at the address. */
	chip_power_on();
	regs[IST8310_WAI] = 0xFF;
	assert(ist8310_probe(&compass, &bus, &c) == -ENODEV);
	assert(strstr(stub_log_last, "WAI at 0x0c is 0xff"));
	assert(allocs == 1 && frees == 1 && reset_count == 0);
	assert(!compass.ops && !compass.priv);
}

static void test_errno(void)
{
	struct ist8310_config c = base_cfg();
	struct compass_dev compass = { 0 };
	struct compass_sample s;

	chip_power_on();
	probe_status = HAL_BUSY;
	assert(ist8310_probe(&compass, &bus, &c) == -EAGAIN);
	assert(allocs == 0 && writes == 0);
	assert(strstr(stub_log_last, "presence check at 0x0c failed (-"));
	chip_power_on();
	probe_status = HAL_TIMEOUT;
	assert(ist8310_probe(&compass, &bus, &c) == -ETIMEDOUT);

	chip_power_on();
	bus_status = HAL_ERROR;
	assert(ist8310_probe(&compass, &bus, &c) == -EIO);
	assert(allocs == 1 && frees == 1 && !compass.ops && !compass.priv);

	compass = open_ok();
	bus_status = HAL_TIMEOUT;
	assert(compass_read(&compass, &s) == -ETIMEDOUT);
	bus_status = HAL_BUSY;
	assert(compass_read(&compass, &s) == -EAGAIN);
	bus_status = HAL_OK;
	ist8310_remove(&compass);
}

static void test_bad_config(void)
{
	struct ist8310_config c;
	struct compass_dev compass = { 0 };

	chip_power_on();
	c = base_cfg();
	c.address = 0x0B;
	assert(ist8310_probe(&compass, &bus, &c) == -EINVAL);
	c.address = 0x10;
	assert(ist8310_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	c.avg_xz = (enum ist8310_avg)5;
	assert(ist8310_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	c.avg_y = (enum ist8310_avg)5;
	assert(ist8310_probe(&compass, &bus, &c) == -EINVAL);
	c = base_cfg();
	assert(ist8310_probe(NULL, &bus, &c) == -EINVAL);
	assert(ist8310_probe(&compass, NULL, &c) == -EINVAL);
	assert(writes == 0 && reads == 0 && probed_addr == 0 && allocs == 0);
	assert(!compass.ops && !compass.priv);
}

static void test_read_cycle(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s = { .t_us = 0 };
	int16_t raw[3];
	bool ready = true;

	field[0] = 100;
	field[1] = -200;
	field[2] = 1000;

	/* Probe's measurement has not landed yet: no data, and no wait. */
	assert(ist8310_data_ready(&compass, &ready) == 0 && !ready);
	stub_tick += MEASURE_MS - 1;
	s.field[0] = 99.0f;
	assert(compass_read(&compass, &s) == -ENODATA);
	assert(s.field[0] == 99.0f && triggers == 1 && "not triggered twice");

	stub_tick += 1;
	assert(ist8310_data_ready(&compass, &ready) == 0 && ready);
	assert(compass_read(&compass, &s) == 0);
	assert(fabs(s.field[0] - 30.0f) < 1e-4);	/* 0.3 uT per count */
	assert(fabs(s.field[1] + 60.0f) < 1e-4);
	assert(fabs(s.field[2] - 300.0f) < 1e-3);
	assert(s.t_us == (int64_t)stub_tick * 1000);
	assert(triggers == 2 && converting && "the next one is under way");

	/* Straight away again: the new one is still measuring. */
	assert(compass_read(&compass, &s) == -ENODATA);
	assert(triggers == 2);

	stub_tick += MEASURE_MS;
	assert(ist8310_read_raw(&compass, raw) == 0);
	assert(raw[0] == 100 && raw[1] == -200 && raw[2] == 1000);
	assert(triggers == 3);
	ist8310_remove(&compass);
}

static void test_clipped(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s = { .t_us = 0 };
	int16_t raw[3];

	/* §4.4: X past 1600 uT is not a reading. */
	field[0] = IST8310_XY_LIMIT + 1;
	stub_tick += MEASURE_MS;
	s.field[0] = 99.0f;
	assert(compass_read(&compass, &s) == -ERANGE);
	assert(s.field[0] == 99.0f && "a refused sample is not written");
	assert(triggers == 2 && "and the next one still started");

	/* read_raw still hands over the counts. */
	stub_tick += MEASURE_MS;
	assert(ist8310_read_raw(&compass, raw) == 0);
	assert(raw[0] == IST8310_XY_LIMIT + 1);

	field[0] = IST8310_XY_LIMIT;
	stub_tick += MEASURE_MS;
	assert(compass_read(&compass, &s) == 0);
	ist8310_remove(&compass);
}

static void test_lost_trigger(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s;

	stub_tick += MEASURE_MS;
	lose_trigger = true;		/* the re-trigger after this read */
	assert(compass_read(&compass, &s) == 0);
	assert(triggers == 1 && !converting);

	/* Nothing will ever land; within 100 ms that is just "not yet". */
	stub_tick += 100;
	assert(compass_read(&compass, &s) == -ENODATA);
	assert(triggers == 1);
	/* Past 100 ms the driver gives up on it and starts another. */
	stub_tick += 1;
	assert(compass_read(&compass, &s) == -ENODATA);
	assert(triggers == 2 && converting);
	stub_tick += MEASURE_MS;
	assert(compass_read(&compass, &s) == 0);

	/*
	 * Data taken through reg_read: the driver still thinks it is
	 * measuring, so it is the same 100 ms recovery.
	 */
	stub_tick += MEASURE_MS;
	{
		uint8_t b;

		assert(ist8310_reg_read(&compass, IST8310_DATA_X_LSB, &b, 1) == 0);
	}
	assert(compass_read(&compass, &s) == -ENODATA && !converting);
	stub_tick += 101;
	assert(compass_read(&compass, &s) == -ENODATA && converting);
	stub_tick += MEASURE_MS;
	assert(compass_read(&compass, &s) == 0);

	/* A trigger the bus refused is retried on the very next read. */
	stub_tick += MEASURE_MS;
	fail_trigger = true;
	assert(compass_read(&compass, &s) == 0 && "the sample still counts");
	assert(!converting);
	fail_trigger = false;
	assert(compass_read(&compass, &s) == -ENODATA);
	assert(converting && "no 100 ms wait this time");
	ist8310_remove(&compass);
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

	assert(ist8310_read_raw(NULL, f) == -EINVAL);
	assert(ist8310_read_raw(&other, f) == -EINVAL);
	assert(ist8310_data_ready(&other, &b) == -EINVAL);
	assert(ist8310_reg_read(&other, 0, &v, 1) == -EINVAL);
	assert(ist8310_reg_write(&other, 0, 0) == -EINVAL);
	ist8310_remove(&other);
	assert(other.ops == &other_ops);

	compass = open_ok();
	assert(ist8310_read_raw(&compass, NULL) == -EINVAL);
	assert(ist8310_data_ready(&compass, NULL) == -EINVAL);
	assert(ist8310_reg_read(&compass, 0, &v, 0) == -EINVAL);
	assert(ist8310_reg_read(&compass, 0, NULL, 1) == -EINVAL);
	reads = 0;
	assert(ist8310_reg_read(&compass, 0, &v, (size_t)UINT16_MAX + 1) ==
	       -EINVAL);
	assert(reads == 0);
	assert(ist8310_reg_write(&compass, 0x20, 0x5A) == 0);
	assert(ist8310_reg_read(&compass, 0x20, &v, 1) == 0 && v == 0x5A);
	ist8310_remove(&compass);
}

int main(void)
{
	test_probe();
	test_absent_and_wrong_chip();
	test_errno();
	test_bad_config();
	test_read_cycle();
	test_clipped();
	test_lost_trigger();
	test_bad_arguments();
	printf("all ist8310 driver checks passed\n");
	return 0;
}
