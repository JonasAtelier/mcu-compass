/*
 * Host-side check of the LIS3MDL driver against a fake chip behind a fake
 * HAL, on I2C and on SPI.
 *
 * The fake decodes the address byte the way the datasheet (DocID024204
 * Rev 4) says the chip does, so a wrong bit shows up as wrong data, not as a
 * pass: on I2C SUB(7) auto-increments (§5.1.1); on SPI the first byte is
 * RW, MS, AD(5:0) (§5.2), and a byte read without MS repeats the one
 * register. It also counts every write that breaks Table 16's or the CTRL
 * tables' rules: a reserved or read-only register, or a must-be-0 bit set.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass.h"
#include "lis3mdl.h"
#include "lis3mdl_math.h"
#include "lis3mdl_regs.h"

#define CS_PIN	0x0010

static I2C_HandleTypeDef i2c;
static SPI_HandleTypeDef spi;
static GPIO_TypeDef cs_port;
static const struct lis3mdl_spi_config spi_cfg = {
	.bus = &spi,
	.cs_port = &cs_port,
	.cs_pin = CS_PIN,
};

static uint8_t regs[0x40];
static bool absent;		/* nothing ACKs on I2C */
static bool spi_empty;		/* nothing on SPI: MISO floats high */
static HAL_StatusTypeDef bus_status;
static uint16_t dev_addr, probed_addr;	/* 7-bit */
static int rule_breaks, writes, allocs, frees;

/* The SPI transaction in progress, between CS low and CS high. */
static bool cs_low;
static int spi_bytes_in;	/* bytes clocked in since CS went low */
static uint8_t spi_reg;
static bool spi_read, spi_auto_increment;

/* Table 16 defaults. */
static void chip_power_on(void)
{
	memset(regs, 0, sizeof(regs));
	regs[LIS3MDL_WHO_AM_I] = LIS3MDL_WHO_AM_I_VALUE;
	regs[LIS3MDL_CTRL_REG1] = 0x10;
	regs[LIS3MDL_CTRL_REG3] = 0x03;
	absent = spi_empty = cs_low = false;
	bus_status = HAL_OK;
	dev_addr = probed_addr = 0;
	rule_breaks = writes = allocs = frees = 0;
}

static void put(int reg, int16_t v)	/* BLE 0: low byte first */
{
	regs[reg] = (uint8_t)(v & 0xFF);
	regs[reg + 1] = (uint8_t)((uint16_t)v >> 8);
}

/* Bits Tables 23, 26, 29 and 32 say must be 0. */
static uint8_t must_be_zero(int reg)
{
	switch (reg) {
	case LIS3MDL_CTRL_REG1:
		return 0x00;
	case LIS3MDL_CTRL_REG2:
		return 0x93;
	case LIS3MDL_CTRL_REG3:
		return 0xD8;
	case LIS3MDL_CTRL_REG4:
		return 0xF1;
	case LIS3MDL_CTRL_REG5:
		return 0x3F;
	default:
		return 0xFF;
	}
}

static void chip_write(int reg, uint8_t value)
{
	writes++;
	/* Only the CTRL registers and INT_CFG/INT_THS are the driver's. */
	if (reg < LIS3MDL_CTRL_REG1 || reg > LIS3MDL_CTRL_REG5 ||
	    (value & must_be_zero(reg))) {
		rule_breaks++;
		return;
	}
	regs[reg] = value;
}

static uint8_t chip_read(int reg)
{
	return reg < (int)sizeof(regs) ? regs[reg] : 0;
}

/* ---------------------------------------------------------------- I2C -- */

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t address,
					uint32_t trials, uint32_t timeout)
{
	assert(h == &i2c && !(address & 1) && trials >= 1 && timeout > 0);
	probed_addr = address >> 1;
	return absent ? HAL_ERROR : bus_status;
}

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t address,
				   uint16_t sub, uint16_t sub_size,
				   uint8_t *rx, uint16_t len, uint32_t timeout)
{
	assert(h == &i2c && sub_size == I2C_MEMADD_SIZE_8BIT && timeout > 0);
	if (absent)
		return HAL_ERROR;
	if (bus_status != HAL_OK)
		return bus_status;
	dev_addr = address >> 1;
	for (int i = 0; i < len; i++)
		rx[i] = chip_read((sub & 0x7F) + ((sub & 0x80) ? i : 0));
	return HAL_OK;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t address,
				    uint16_t sub, uint16_t sub_size,
				    uint8_t *data, uint16_t len,
				    uint32_t timeout)
{
	assert(h == &i2c && sub_size == I2C_MEMADD_SIZE_8BIT && timeout > 0);
	if (absent)
		return HAL_ERROR;
	if (bus_status != HAL_OK)
		return bus_status;
	dev_addr = address >> 1;
	for (int i = 0; i < len; i++)
		chip_write((sub & 0x7F) + ((sub & 0x80) ? i : 0), data[i]);
	return HAL_OK;
}

/* ---------------------------------------------------------------- SPI -- */

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &cs_port && pin == CS_PIN);
	if (state == GPIO_PIN_RESET) {
		assert(!cs_low && "CS was already low");
		cs_low = true;
		spi_bytes_in = 0;
	} else {
		assert(cs_low && "CS went high twice");
		cs_low = false;
	}
}

/*
 * The register the next data byte goes to or comes from: with MS, one on
 * per byte already moved after the address byte; without, the same one.
 */
static int spi_target(void)
{
	return spi_reg + (spi_auto_increment ? spi_bytes_in - 1 : 0);
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *data,
				   uint16_t len, uint32_t timeout)
{
	assert(h == &spi && cs_low && timeout > 0);
	if (bus_status != HAL_OK)
		return bus_status;
	for (int i = 0; i < len; i++) {
		if (spi_bytes_in == 0) {
			/* §5.2: RW, MS, AD(5:0), MSb first. */
			spi_read = data[i] & 0x80;
			spi_auto_increment = data[i] & 0x40;
			spi_reg = data[i] & 0x3F;
			spi_bytes_in++;
			continue;
		}
		assert(!spi_read && "data clocked in during a read");
		chip_write(spi_target(), data[i]);
		spi_bytes_in++;
	}
	return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *data,
				  uint16_t len, uint32_t timeout)
{
	assert(h == &spi && cs_low && timeout > 0);
	assert(spi_bytes_in >= 1 && spi_read && "read without RW set");
	if (bus_status != HAL_OK)
		return bus_status;
	for (int i = 0; i < len; i++) {
		data[i] = spi_empty ? 0xFF : chip_read(spi_target());
		spi_bytes_in++;
	}
	return HAL_OK;
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

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-3f;
}

/* -------------------------------------------------------------- tests -- */

static void test_probe_defaults(void)
{
	struct compass_dev compass = { 0 };

	chip_power_on();
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == 0);
	assert(probed_addr == LIS3MDL_ADDR_HIGH && dev_addr == LIS3MDL_ADDR_HIGH);
	assert(strcmp(compass.name, "lis3mdl") == 0);
	/* Chip defaults (Table 16) kept, plus TEMP_EN, BDU, continuous. */
	assert(regs[LIS3MDL_CTRL_REG1] == 0x90);
	assert(regs[LIS3MDL_CTRL_REG2] == 0x00);
	assert(regs[LIS3MDL_CTRL_REG3] == 0x00);
	assert(regs[LIS3MDL_CTRL_REG4] == 0x00);
	assert(regs[LIS3MDL_CTRL_REG5] == 0x40);
	assert(rule_breaks == 0);
	assert(strstr(stub_log_last, "at 0x1e: +/-4 gauss, 10 Hz, low-power"));
	lis3mdl_remove(&compass);
	assert(regs[LIS3MDL_CTRL_REG3] == LIS3MDL_MD_POWER_DOWN);
	assert(!compass.ops && !compass.priv && allocs == frees);
}

static void test_probe_config(void)
{
	struct lis3mdl_config cfg = {
		.address = LIS3MDL_ADDR_LOW,
		.range = LIS3MDL_RANGE_1600UT,
		.odr = LIS3MDL_ODR_80HZ,
		.performance = LIS3MDL_ULTRA_HIGH,
	};
	struct compass_dev compass = { 0 };

	chip_power_on();
	assert(lis3mdl_probe_i2c(&compass, &i2c, &cfg) == 0);
	assert(dev_addr == LIS3MDL_ADDR_LOW);
	/* Tables 18-22, 25, 31. */
	assert(regs[LIS3MDL_CTRL_REG1] == (0x80 | 0x60 | 0x1C));
	assert(regs[LIS3MDL_CTRL_REG2] == 0x60);
	assert(regs[LIS3MDL_CTRL_REG4] == 0x0C);
	assert(rule_breaks == 0);
	lis3mdl_remove(&compass);
}

static void test_probe_refusals(void)
{
	struct lis3mdl_config cfg = { 0 };
	struct compass_dev compass = { 0 };

	/* Nothing ACKs: -ENXIO, nothing allocated, debug line only. */
	chip_power_on();
	absent = true;
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == -ENXIO);
	assert(allocs == 0 && !compass.ops && !compass.priv);

	/* A busy or stuck bus keeps its own errno. */
	chip_power_on();
	bus_status = HAL_BUSY;
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == -EAGAIN);
	assert(allocs == 0);

	/* Something else at the address. */
	chip_power_on();
	regs[LIS3MDL_WHO_AM_I] = 0x33;
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == -ENODEV);
	assert(allocs == 1 && frees == 1 && writes == 0);
	assert(!compass.ops && !compass.priv);

	/* Bad arguments, all refused before the bus. */
	chip_power_on();
	assert(lis3mdl_probe_i2c(NULL, &i2c, NULL) == -EINVAL);
	assert(lis3mdl_probe_i2c(&compass, NULL, NULL) == -EINVAL);
	cfg.address = 0x1D;
	assert(lis3mdl_probe_i2c(&compass, &i2c, &cfg) == -EINVAL);
	cfg.address = 0;
	cfg.range = (enum lis3mdl_range)4;
	assert(lis3mdl_probe_i2c(&compass, &i2c, &cfg) == -EINVAL);
	cfg.range = LIS3MDL_RANGE_400UT;
	cfg.odr = (enum lis3mdl_odr)8;
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, &cfg) == -EINVAL);
	cfg.odr = LIS3MDL_ODR_10HZ;
	cfg.performance = (enum lis3mdl_performance)4;
	assert(lis3mdl_probe_i2c(&compass, &i2c, &cfg) == -EINVAL);
	assert(lis3mdl_probe_spi(&compass, NULL, NULL) == -EINVAL);
	assert(allocs == 0 && probed_addr == 0);

	/* Bound already. */
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == 0);
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == -EBUSY);
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, NULL) == -EBUSY);
	lis3mdl_remove(&compass);
	assert(allocs == frees);
}

/* The same field through either bus, with auto-increment honoured. */
static void check_read(struct compass_dev *compass)
{
	struct compass_sample s;

	/* 1 gauss on X, -0.5 on Y, 0.25 on Z at +/-4 gauss. */
	put(LIS3MDL_OUT_X_L, 6842);
	put(LIS3MDL_OUT_X_L + 2, -3421);
	put(LIS3MDL_OUT_X_L + 4, 1711);
	assert(compass_read(compass, &s) == 0);
	assert(near(s.field[0], 100.0f));
	assert(near(s.field[1], -50.0f));
	assert(fabsf(s.field[2] - 25.0f) < 0.01f);

	/* Past full scale is clipped, and the sample is left alone. */
	put(LIS3MDL_OUT_X_L + 2, -(4 * 6842 + 1));
	s.field[0] = 42.0f;
	assert(compass_read(compass, &s) == -ERANGE);
	assert(s.field[0] == 42.0f);
}

static void test_read_i2c(void)
{
	struct compass_dev compass = { 0 };

	chip_power_on();
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == 0);
	check_read(&compass);
	lis3mdl_remove(&compass);
}

static void test_spi(void)
{
	struct compass_dev compass = { 0 };
	struct lis3mdl_config cfg = { .range = LIS3MDL_RANGE_800UT };
	struct compass_sample s;

	chip_power_on();
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, NULL) == 0);
	assert(!cs_low && rule_breaks == 0);
	assert(regs[LIS3MDL_CTRL_REG3] == 0x00);	/* SIM 0: 4-wire */
	assert(strstr(stub_log_last, "on SPI: +/-4 gauss"));
	assert(probed_addr == 0);	/* no I2C traffic at all */
	check_read(&compass);
	lis3mdl_remove(&compass);
	assert(!cs_low && allocs == frees);

	/* A range the probe sets is the range the read uses. */
	chip_power_on();
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, &cfg) == 0);
	put(LIS3MDL_OUT_X_L, 3421);
	put(LIS3MDL_OUT_X_L + 2, 0);
	put(LIS3MDL_OUT_X_L + 4, 0);
	assert(compass_read(&compass, &s) == 0 && near(s.field[0], 100.0f));
	lis3mdl_remove(&compass);

	/* Nothing on the bus: MISO reads 0xFF, so WHO_AM_I decides. */
	chip_power_on();
	spi_empty = true;
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, NULL) == -ENODEV);
	assert(strstr(stub_log_last, "WHO_AM_I on SPI is 0xff"));
	assert(!cs_low && allocs == 1 && frees == 1 && writes == 0);

	/* A bus failure comes back as its errno, CS released. */
	chip_power_on();
	bus_status = HAL_TIMEOUT;
	assert(lis3mdl_probe_spi(&compass, &spi_cfg, NULL) == -ETIMEDOUT);
	assert(!cs_low && allocs == frees);

	/* A NULL handle or chip-select port is refused. */
	{
		struct lis3mdl_spi_config bad = spi_cfg;

		bad.cs_port = NULL;
		assert(lis3mdl_probe_spi(&compass, &bad, NULL) == -EINVAL);
	}
}

static void test_extras(void)
{
	struct compass_dev compass = { 0 }, other = { 0 };
	struct compass_sample s;
	int16_t raw[3];
	uint8_t v = 0;
	float t;
	bool ready;

	chip_power_on();
	assert(lis3mdl_probe_i2c(&compass, &i2c, NULL) == 0);

	put(LIS3MDL_OUT_X_L, 1);
	put(LIS3MDL_OUT_X_L + 2, -2);
	put(LIS3MDL_OUT_X_L + 4, 30000);	/* clipped, but raw is raw */
	assert(lis3mdl_read_raw(&compass, raw) == 0);
	assert(raw[0] == 1 && raw[1] == -2 && raw[2] == 30000);

	regs[LIS3MDL_STATUS_REG] = LIS3MDL_ZYXDA;
	assert(lis3mdl_data_ready(&compass, &ready) == 0 && ready);
	regs[LIS3MDL_STATUS_REG] = 0x07;	/* single axes only */
	assert(lis3mdl_data_ready(&compass, &ready) == 0 && !ready);

	put(LIS3MDL_TEMP_OUT_L, 80);
	assert(lis3mdl_temperature(&compass, &t) == 0 && near(t, 10.0f));

	/* Range: CTRL_REG2 and the conversion move together. */
	assert(lis3mdl_set_range(&compass, LIS3MDL_RANGE_1200UT) == 0);
	assert(regs[LIS3MDL_CTRL_REG2] == 0x40);
	put(LIS3MDL_OUT_X_L, 2281);
	put(LIS3MDL_OUT_X_L + 4, 0);
	assert(compass_read(&compass, &s) == 0 && near(s.field[0], 100.0f));
	assert(lis3mdl_set_range(&compass, (enum lis3mdl_range)4) == -EINVAL);
	/* A failed write keeps the old range. */
	bus_status = HAL_ERROR;
	assert(lis3mdl_set_range(&compass, LIS3MDL_RANGE_400UT) == -EIO);
	bus_status = HAL_OK;
	assert(compass_read(&compass, &s) == 0 && near(s.field[0], 100.0f));

	assert(lis3mdl_standby(&compass, true) == 0);
	assert(regs[LIS3MDL_CTRL_REG3] == LIS3MDL_MD_POWER_DOWN);
	assert(lis3mdl_standby(&compass, false) == 0);
	assert(regs[LIS3MDL_CTRL_REG3] == LIS3MDL_MD_CONTINUOUS);

	/* The escape hatch, and its limits. */
	assert(lis3mdl_reg_read(&compass, LIS3MDL_WHO_AM_I, &v, 1) == 0);
	assert(v == LIS3MDL_WHO_AM_I_VALUE);
	assert(lis3mdl_reg_write(&compass, LIS3MDL_CTRL_REG5, 0x00) == 0);
	assert(regs[LIS3MDL_CTRL_REG5] == 0x00);
	assert(lis3mdl_reg_read(&compass, 0x40, &v, 1) == -EINVAL);
	assert(lis3mdl_reg_read(&compass, 0x0F, &v, 0) == -EINVAL);
	assert(lis3mdl_reg_write(&compass, 0x40, 0) == -EINVAL);
	assert(rule_breaks == 0);

	/* Someone else's device, or none. */
	assert(lis3mdl_read_raw(&other, raw) == -EINVAL);
	assert(lis3mdl_set_range(NULL, LIS3MDL_RANGE_400UT) == -EINVAL);
	assert(lis3mdl_data_ready(&compass, NULL) == -EINVAL);
	lis3mdl_remove(&other);		/* safe on an unprobed device */

	lis3mdl_remove(&compass);
	assert(allocs == frees);
}

int main(void)
{
	test_probe_defaults();
	test_probe_config();
	test_probe_refusals();
	test_read_i2c();
	test_spi();
	test_extras();
	printf("all lis3mdl driver checks passed\n");
	return 0;
}
