/*
 * Host-side check of compass_port_esp.c, the ESP-IDF side of compass_priv.h,
 * against test/idf_stubs. The chip logic is the same C on both platforms and
 * is tested once, on the HAL stub, by the other test_*.c files; this file
 * covers only what ESP-IDF does differently: esp_err_t -> errno, the I2C or
 * SPI device each probe adds to the bus, that remove (or a failed probe)
 * takes it off again, and how each bus kind frames a transfer.
 *
 * QMC5883L stands in for every I2C chip: they all reach the bus the same
 * way. SPI and UART are driven through the ops directly.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "cmps14.h"
#include "compass_priv.h"
#include "lis3mdl.h"
#include "lis3mdl_regs.h"
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

static uint8_t i2c_sent[8];	/* the last i2c_master_transmit() */
static size_t i2c_sent_len;

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *data,
			      size_t len, int t)
{
	(void)d;
	(void)t;
	assert(len >= 1 && len <= sizeof(i2c_sent));
	if (next_err != ESP_OK)
		return next_err;
	memcpy(i2c_sent, data, len);
	i2c_sent_len = len;
	if (!floating_bus && len == 2)
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

/* ---------------------------------------------------- fake SPI device -- */

static spi_device_interface_config_t spi_cfg;
static spi_host_device_t spi_host;
static int spi_devices;		/* added and not yet removed */
static uint8_t spi_out[64];	/* what the last transaction clocked out */
static size_t spi_bits;
static uint8_t spi_reply[64];	/* what the device clocks back, per byte */

#define SPI_DEV ((spi_device_handle_t)(void *)spi_reply)

esp_err_t spi_bus_add_device(spi_host_device_t host,
			     const spi_device_interface_config_t *cfg,
			     spi_device_handle_t *handle)
{
	if (next_err != ESP_OK)
		return next_err;
	spi_host = host;
	spi_cfg = *cfg;
	spi_devices++;
	*handle = SPI_DEV;
	return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{
	assert(handle == SPI_DEV);
	spi_devices--;
	return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
				      spi_transaction_t *t)
{
	assert(handle == SPI_DEV);
	assert(t->length % 8 == 0 && t->length / 8 <= sizeof(spi_out));
	if (next_err != ESP_OK)
		return next_err;
	spi_bits = t->length;
	memcpy(spi_out, t->tx_buffer, t->length / 8);
	memcpy(t->rx_buffer, spi_reply, t->length / 8);
	return ESP_OK;
}

/* --------------------------------------------------- fake UART device -- */

#define UART_PORT 1

static bool uart_installed;
static char uart_calls[8];	/* F flush, W write, R read, in order */
static uint8_t uart_out[32];	/* what the last write sent */
static size_t uart_out_len;
static uint8_t uart_in[32];	/* what is waiting to be read */
static size_t uart_in_len;
static uint8_t uart_reply[32];	/* what the device sends after a write */
static size_t uart_reply_len;
static TickType_t uart_waited;	/* ticks_to_wait of the last read */
static int uart_write_short, uart_read_error;

static void uart_reset(void)
{
	uart_installed = true;
	uart_calls[0] = '\0';
	uart_out_len = uart_in_len = uart_reply_len = 0;
	uart_write_short = uart_read_error = 0;
	next_err = ESP_OK;
}

static void uart_called(char c)
{
	size_t n = strlen(uart_calls);

	assert(n + 1 < sizeof(uart_calls));
	uart_calls[n] = c;
	uart_calls[n + 1] = '\0';
}

bool uart_is_driver_installed(uart_port_t port)
{
	return uart_installed && port == UART_PORT;
}

esp_err_t uart_flush_input(uart_port_t port)
{
	assert(port == UART_PORT);
	uart_called('F');
	if (next_err != ESP_OK)
		return next_err;
	uart_in_len = 0;
	return ESP_OK;
}

int uart_write_bytes(uart_port_t port, const void *src, size_t size)
{
	assert(port == UART_PORT && size <= sizeof(uart_out));
	uart_called('W');
	if (uart_write_short)
		return 0;
	memcpy(uart_out, src, size);
	uart_out_len = size;
	/* The device answers after the command: queue its reply. */
	memcpy(uart_in + uart_in_len, uart_reply, uart_reply_len);
	uart_in_len += uart_reply_len;
	return (int)size;
}

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length,
		    TickType_t ticks_to_wait)
{
	size_t n = length < uart_in_len ? length : uart_in_len;

	assert(port == UART_PORT);
	uart_called('R');
	uart_waited = ticks_to_wait;
	if (uart_read_error)
		return -1;
	memcpy(buf, uart_in, n);
	memmove(uart_in, uart_in + n, uart_in_len - n);
	uart_in_len -= n;
	return (int)n;
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
	struct compass_bus bus;
	uint8_t b;

	chip_reset();
	assert(compass_i2c_attach(&bus, BUS, QMC5883L_ADDR, 0, 10) == 0);
	assert(bus.ops->kind == COMPASS_BUS_I2C);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		next_err = map[i].err;
		assert(compass_i2c_probe(BUS, 0x2C, 10) == map[i].expected);
		assert(probed_addr == 0x2C);
		assert(compass_reg_read(&bus, 0, &b, 1) == map[i].expected);
		assert(compass_reg_write(&bus, 0, 0) == map[i].expected);
	}
	next_err = ESP_OK;
	compass_bus_detach(&bus);
	assert(attached == 0);
}

/* I2C framing: a register write is one transmit, a bare cmd one byte. */
static void test_i2c_transfer(void)
{
	static const uint8_t two[2] = { 0xAA, 0xBB };
	uint8_t many[COMPASS_BUS_MAX_TX + 1] = { 0 }, b;
	struct compass_bus bus;

	chip_reset();
	assert(compass_i2c_attach(&bus, BUS, QMC5883L_ADDR, 0, 10) == 0);
	assert(bus.ops->transfer(&bus, 0x21, two, 2, NULL, 0) == 0);
	assert(i2c_sent_len == 3 && i2c_sent[0] == 0x21 &&
	       i2c_sent[1] == 0xAA && i2c_sent[2] == 0xBB);
	assert(bus.ops->transfer(&bus, 0x7E, NULL, 0, NULL, 0) == 0);
	assert(i2c_sent_len == 1 && i2c_sent[0] == 0x7E);
	/* Write-then-read is not one I2C transaction here; too long is out. */
	assert(bus.ops->transfer(&bus, 0x21, two, 1, &b, 1) == -EINVAL);
	assert(bus.ops->transfer(&bus, 0x21, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	compass_bus_detach(&bus);
}

static void test_scl_hz(void)
{
	struct compass_dev compass = { 0 };
	struct qmc5883l_config cfg = { 0 };
	struct compass_bus bus;

	/* The port: 0 selects 400 kHz, anything else goes through. */
	chip_reset();
	assert(compass_i2c_attach(&bus, BUS, 0x0E, 0, 10) == 0);
	assert(dev_addr == 0x0E && dev_hz == 400000);
	compass_bus_detach(&bus);
	assert(compass_i2c_attach(&bus, BUS, 0x0E, 100000, 10) == 0);
	assert(dev_hz == 100000);
	compass_bus_detach(&bus);

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

static void test_spi(void)
{
	static const uint8_t two[2] = { 0xAA, 0xBB };
	struct compass_bus bus;
	uint8_t rx[3] = { 0 };

	next_err = ESP_OK;
	assert(compass_spi_attach(&bus, SPI2_HOST, 10, 5000000, 3, 10) == 0);
	assert(bus.ops->kind == COMPASS_BUS_SPI);
	assert(spi_devices == 1 && spi_host == SPI2_HOST);
	assert(spi_cfg.spics_io_num == 10 && spi_cfg.mode == 3 &&
	       spi_cfg.clock_speed_hz == 5000000 && spi_cfg.queue_size == 1);

	/* A read: cmd and three clocks of zeros, rx is what came back after. */
	memcpy(spi_reply, "\x99\x01\x02\x03", 4);
	assert(bus.ops->transfer(&bus, 0xE8, NULL, 0, rx, 3) == 0);
	assert(spi_bits == 32);
	assert(spi_out[0] == 0xE8 && !spi_out[1] && !spi_out[2] && !spi_out[3]);
	assert(rx[0] == 1 && rx[1] == 2 && rx[2] == 3);

	/* A write: cmd, then the data, in one CS window. */
	assert(compass_reg_write(&bus, 0x20, 0x5A) == 0);
	assert(spi_bits == 16 && spi_out[0] == 0x20 && spi_out[1] == 0x5A);

	/* Both: rx starts after cmd and tx. */
	assert(bus.ops->transfer(&bus, 0x10, two, 2, rx, 1) == 0);
	assert(spi_bits == 32 && spi_out[1] == 0xAA && spi_out[2] == 0xBB);
	assert(rx[0] == 0x03);

	/* More than one transaction holds, and a failed one leaves rx alone. */
	assert(bus.ops->transfer(&bus, 0, NULL, 0, rx, 32) == -EINVAL);
	rx[0] = 0x42;
	next_err = ESP_ERR_TIMEOUT;
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, rx, 1) == -ETIMEDOUT);
	assert(rx[0] == 0x42);
	next_err = ESP_OK;

	compass_bus_detach(&bus);
	assert(spi_devices == 0);

	next_err = ESP_ERR_NO_MEM;
	assert(compass_spi_attach(&bus, SPI2_HOST, 10, 5000000, 3, 10) ==
	       -ENOMEM);
	next_err = ESP_OK;
	assert(spi_devices == 0);
}

/* The LIS3MDL's ESP-only SPI choices: mode 3, 5 MHz unless asked, <= 10. */
static void test_lis3mdl_spi(void)
{
	struct lis3mdl_spi_config spi = { .host = SPI2_HOST, .cs_pin = 7 };
	struct compass_dev compass = { 0 };

	next_err = ESP_OK;
	memset(spi_reply, 0, sizeof(spi_reply));
	spi_reply[1] = LIS3MDL_WHO_AM_I_VALUE;	/* after the address byte */
	assert(lis3mdl_probe_spi(&compass, &spi, NULL) == 0);
	assert(spi_devices == 1 && spi_cfg.mode == 3 &&
	       spi_cfg.clock_speed_hz == 5000000 && spi_cfg.spics_io_num == 7);
	lis3mdl_remove(&compass);
	assert(spi_devices == 0);
	/* Removing powers it down: CTRL_REG3 = 11b, a plain write. */
	assert(spi_bits == 16 && spi_out[0] == LIS3MDL_CTRL_REG3 &&
	       spi_out[1] == LIS3MDL_MD_POWER_DOWN);

	spi.clock_hz = 10000000;
	assert(lis3mdl_probe_spi(&compass, &spi, NULL) == 0);
	assert(spi_cfg.clock_speed_hz == 10000000);
	lis3mdl_remove(&compass);
	spi.clock_hz = 10000001;
	assert(lis3mdl_probe_spi(&compass, &spi, NULL) == -EINVAL);
	assert(spi_devices == 0);

	/* A wrong WHO_AM_I takes the device off the bus again. */
	spi.clock_hz = 0;
	spi_reply[1] = 0xFF;
	assert(lis3mdl_probe_spi(&compass, &spi, NULL) == -ENODEV);
	assert(spi_devices == 0 && !compass.ops);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t many[COMPASS_BUS_MAX_TX + 1] = { 0 };
	struct compass_bus bus;
	uint8_t rx[2] = { 0 };

	/* The driver is the app's to install; without it, refuse. */
	uart_reset();
	uart_installed = false;
	assert(compass_uart_attach(&bus, UART_PORT, 100) == -EINVAL);
	uart_installed = true;
	assert(compass_uart_attach(&bus, UART_PORT, 100) == 0);
	assert(bus.ops->kind == COMPASS_BUS_UART);

	/* A stale byte is dropped, then cmd and args go out, then the reply. */
	uart_in[0] = 0xEE;
	uart_in_len = 1;
	memcpy(uart_reply, "\x12\x34", 2);
	uart_reply_len = 2;
	stub_tick_hz = 100;
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(uart_calls, "FWR") == 0);
	assert(uart_out_len == 3 && uart_out[0] == 0x13 &&
	       uart_out[1] == 0x11 && uart_out[2] == 0x22);
	assert(rx[0] == 0x12 && rx[1] == 0x34);
	/* 100 ms at 100 Hz: 10 ticks, plus one for the partial first. */
	assert(uart_waited == 11);

	/* A command with no reply does not read. */
	uart_reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(uart_calls, "FW") == 0);

	/* A reply that stops short is a timeout. */
	uart_reset();
	uart_reply[0] = 0x12;
	uart_reply_len = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);

	/* Driver failures. */
	uart_reset();
	uart_read_error = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	uart_reset();
	uart_write_short = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EIO);
	assert(strcmp(uart_calls, "FW") == 0);
	uart_reset();
	next_err = ESP_ERR_INVALID_ARG;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EINVAL);
	assert(strcmp(uart_calls, "F") == 0);
	uart_reset();
	assert(bus.ops->transfer(&bus, 0x13, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(uart_calls[0] == '\0');

	compass_bus_detach(&bus);
}

/* The CMPS14 on an ESP-IDF port: the app's driver, or nothing. */
static void test_cmps14(void)
{
	struct compass_dev compass = { 0 };
	uint8_t v = 0;

	uart_reset();
	uart_installed = false;
	assert(cmps14_probe(&compass, UART_PORT) == -EINVAL);

	uart_reset();
	uart_reply[0] = 9;	/* p.6: GET VERSION answers one byte */
	uart_reply_len = 1;
	assert(cmps14_probe(&compass, UART_PORT) == 0);
	assert(uart_out_len == 1 && uart_out[0] == 0x11);
	assert(cmps14_version(&compass, &v) == 0 && v == 9);
	cmps14_remove(&compass);

	/* Silence is -ENXIO. */
	uart_reset();
	assert(cmps14_probe(&compass, UART_PORT) == -ENXIO);
	assert(!compass.ops && !compass.priv);
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
	test_i2c_transfer();
	test_scl_hz();
	test_device_released();
	test_spi();
	test_lis3mdl_spi();
	test_uart();
	test_cmps14();
	test_clock_and_sleep();
	printf("test_port_esp: all passed\n");
	return 0;
}
