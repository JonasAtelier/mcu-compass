/*
 * Host-side check of compass_port_esp.c, the ESP-IDF side of compass_priv.h,
 * against test/idf_stubs. The chip logic is the same C on both platforms and
 * is tested once, on the HAL stub, by the other test_*.c files; this file
 * covers only what ESP-IDF does differently: esp_err_t -> errno, the device
 * each probe adds to the bus at scl_hz, and that remove (or a failed probe)
 * takes it off again.
 *
 * QMC5883L stands in for every chip: they all reach the bus the same way.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_priv.h"
#include "qmc5883l.h"
#include "qmc5883l_regs.h"

static uint8_t regs[256];
static uint16_t dev_addr, probed_addr;
static uint32_t dev_hz;
static esp_err_t next_err;	/* what the next bus call returns */
static esp_err_t add_err;	/* what i2c_master_bus_add_device() returns */
static bool floating_bus;	/* answers, but every read gives 0xFF */
static int attached;		/* devices added and not yet removed */
static TickType_t asked_ticks;	/* the last vTaskDelay() argument */

#define BUS ((i2c_master_bus_handle_t)(void *)regs)

static void chip_reset(void)
{
	memset(regs, 0, sizeof(regs));
	regs[QMC5883L_CHIP_ID] = QMC5883L_CHIP_ID_VALUE;
	next_err = add_err = ESP_OK;
	floating_bus = false;
	attached = 0;
	dev_hz = 0;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
				    const i2c_device_config_t *cfg,
				    i2c_master_dev_handle_t *out)
{
	(void)bus;
	if (next_err != ESP_OK)
		return next_err;
	if (add_err != ESP_OK)
		return add_err;
	assert(cfg->dev_addr_length == I2C_ADDR_BIT_LEN_7);
	dev_addr = cfg->device_address;
	dev_hz = cfg->scl_speed_hz;
	attached++;
	*out = (i2c_master_dev_handle_t)(void *)regs;
	return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d)
{
	assert(d == (i2c_master_dev_handle_t)(void *)regs);
	attached--;
	return ESP_OK;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address,
			   int xfer_timeout_ms)
{
	(void)bus;
	(void)xfer_timeout_ms;
	probed_addr = address;
	return next_err;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *data,
			      size_t len, int t)
{
	(void)d;
	(void)t;
	assert(len == 2);
	if (next_err != ESP_OK)
		return next_err;
	if (!floating_bus)
		regs[data[0]] = data[1];
	return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d,
				      const uint8_t *tx, size_t tx_len,
				      uint8_t *rx, size_t rx_len, int t)
{
	(void)d;
	(void)t;
	assert(tx_len == 1);
	if (next_err != ESP_OK)
		return next_err;
	for (size_t i = 0; i < rx_len; i++)
		rx[i] = floating_bus ? 0xFF : regs[(tx[0] + i) & 0xFF];
	return ESP_OK;
}

void vTaskDelay(TickType_t t)
{
	asked_ticks = t;
}

TickType_t xTaskGetTickCount(void)
{
	return 0;
}

static void test_errno_mapping(void)
{
	static const struct {
		esp_err_t err;
		int expected;
	} map[] = {
		{ ESP_OK,		 0 },
		{ ESP_ERR_INVALID_ARG,	 -EINVAL },
		{ ESP_ERR_TIMEOUT,	 -ETIMEDOUT },
		{ ESP_ERR_NOT_FOUND,	 -ENXIO },	/* nothing ACKed */
		{ ESP_ERR_INVALID_SIZE,	 -EMSGSIZE },
		{ ESP_ERR_NO_MEM,	 -ENOMEM },
		{ ESP_ERR_INVALID_STATE, -EIO },	/* IDF 5.5: NACK mid-transfer */
		{ ESP_FAIL,		 -EIO },
	};
	struct compass_i2c i2c;
	uint8_t b;

	chip_reset();
	assert(compass_i2c_attach(&i2c, BUS, QMC5883L_ADDR, 0) == 0);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		next_err = map[i].err;
		assert(compass_i2c_probe(BUS, 0x2C, 10) == map[i].expected);
		assert(probed_addr == 0x2C);
		assert(compass_i2c_read(&i2c, 0, &b, 1, 10) ==
		       map[i].expected);
		assert(compass_i2c_write(&i2c, 0, 0, 10) == map[i].expected);
	}
	next_err = ESP_OK;
	compass_i2c_detach(&i2c);
	assert(attached == 0);
}

static void test_scl_hz(void)
{
	struct compass_dev compass = { 0 };
	struct qmc5883l_config cfg = { 0 };
	struct compass_i2c i2c;

	/* The port: 0 selects 400 kHz, anything else goes through. */
	chip_reset();
	assert(compass_i2c_attach(&i2c, BUS, 0x0E, 0) == 0);
	assert(dev_addr == 0x0E && dev_hz == 400000);
	compass_i2c_detach(&i2c);
	assert(compass_i2c_attach(&i2c, BUS, 0x0E, 100000) == 0);
	assert(dev_hz == 100000);
	compass_i2c_detach(&i2c);

	/* A driver passes cfg->scl_hz on, and NULL cfg is 400 kHz. */
	assert(qmc5883l_probe(&compass, BUS, NULL) == 0);
	assert(dev_addr == QMC5883L_ADDR && dev_hz == 400000);
	qmc5883l_remove(&compass);

	cfg.scl_hz = 100000;
	assert(qmc5883l_probe(&compass, BUS, &cfg) == 0);
	assert(dev_hz == 100000);
	qmc5883l_remove(&compass);
}

static void test_device_released(void)
{
	struct compass_dev compass = { 0 };

	/* remove takes the device off the bus. */
	chip_reset();
	assert(qmc5883l_probe(&compass, BUS, NULL) == 0);
	assert(attached == 1);
	/* Logged under the one "compass" tag, prefixed with the chip. */
	assert(strncmp(stub_log_last, "I compass: qmc5883l: at 0x0d", 28) == 0);
	qmc5883l_remove(&compass);
	assert(attached == 0);
	assert(!compass.ops && !compass.priv);

	/* So does a probe that fails after adding it. */
	chip_reset();
	floating_bus = true;
	assert(qmc5883l_probe(&compass, BUS, NULL) == -ENODEV);
	assert(attached == 0);
	assert(!compass.ops && !compass.priv);

	/* A refused add: the chip ACKed, the bus would not take it. */
	chip_reset();
	add_err = ESP_ERR_NO_MEM;
	assert(qmc5883l_probe(&compass, BUS, NULL) == -ENOMEM);
	assert(attached == 0);
	assert(!compass.ops && !compass.priv);
	assert(strstr(stub_log_last, "cannot add 0x0d to the bus"));
}

static void test_clock_and_sleep(void)
{
	stub_time_us = 123456789;
	assert(compass_time_us() == 123456789);
	assert(compass_tick_ms() == 123456);

	/* Rounded up, plus one for the partial first tick. */
	stub_tick_hz = 100;
	compass_sleep_ms(2);
	assert(asked_ticks == 2);
	compass_sleep_ms(15);
	assert(asked_ticks == 3);
	stub_tick_hz = 1000;
	compass_sleep_ms(10);
	assert(asked_ticks == 11);
	stub_tick_hz = 100;
}

int main(void)
{
	test_errno_mapping();
	test_scl_hz();
	test_device_released();
	test_clock_and_sleep();
	printf("test_port_esp: all passed\n");
	return 0;
}
