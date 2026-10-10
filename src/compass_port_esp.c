/*
 * compass_priv.h on ESP-IDF. The CMakeLists lists this file; stm-forge
 * compiles every .c in src/, so outside ESP-IDF it is empty.
 */
#ifdef ESP_PLATFORM

#include <string.h>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_priv.h"

/*
 * Fast mode, the most the parts here take: QMC5883P §5.4, IST8310 §6.1,
 * LIS3MDL DS DocID024204 Rev 4 Table 7.
 */
#define DEFAULT_SCL_HZ		400000

/* One SPI transaction: cmd, tx and rx together. */
#define COMPASS_SPI_MAX_LEN	32

/*
 * esp_err_t -> negative errno, at the bus boundary and nowhere else.
 * i2c_master_probe() reports a missing ACK as ESP_ERR_NOT_FOUND, which is
 * -ENXIO. A NACK in the middle of a transfer comes back from IDF 5.5 as
 * ESP_ERR_INVALID_STATE, which lands on -EIO.
 */
static int compass_errno(esp_err_t err)
{
	switch (err) {
	case ESP_OK:
		return 0;
	case ESP_ERR_INVALID_ARG:
		return -EINVAL;
	case ESP_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case ESP_ERR_NOT_FOUND:
		return -ENXIO;
	case ESP_ERR_INVALID_SIZE:
		return -EMSGSIZE;
	case ESP_ERR_NO_MEM:
		return -ENOMEM;
	default:
		return -EIO;
	}
}

int64_t compass_time_us(void)
{
	return esp_timer_get_time();
}

uint32_t compass_tick_ms(void)
{
	return (uint32_t)(esp_timer_get_time() / 1000);
}

/*
 * Two things make a plain vTaskDelay(pdMS_TO_TICKS(ms)) wait less than asked:
 *
 *   - pdMS_TO_TICKS() truncates. At IDF's default 100 Hz tick, 2 ms is 0
 *     ticks and 15 ms is 1, so round up instead.
 *   - vTaskDelay(n) counts tick interrupts, and the first one may be about
 *     to fire, so n ticks can be as little as n - 1 full periods. Add one.
 *
 * So 2 ms at 100 Hz asks for 2 ticks and waits 10-20 ms: long, never short.
 */
static TickType_t compass_ticks(uint32_t ms)
{
	return (TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999) / 1000) +
	       1;
}

void compass_sleep_ms(uint32_t ms)
{
	vTaskDelay(compass_ticks(ms));
}

int compass_i2c_probe(compass_i2c_bus handle, uint8_t address,
		      uint32_t timeout_ms)
{
	return compass_errno(i2c_master_probe(handle, address,
					      (int)timeout_ms));
}

/* ------------------------------------------------------------------ I2C -- */

static int compass_i2c_transfer(struct compass_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	uint8_t out[1 + COMPASS_BUS_MAX_TX];

	if ((tx_len && rx_len) || tx_len > COMPASS_BUS_MAX_TX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);
	if (rx_len)
		return compass_errno(i2c_master_transmit_receive(
			bus->i2c, out, 1, rx, rx_len, (int)bus->timeout_ms));
	return compass_errno(i2c_master_transmit(bus->i2c, out, 1 + tx_len,
						 (int)bus->timeout_ms));
}

static void compass_i2c_detach(struct compass_bus *bus)
{
	i2c_master_bus_rm_device(bus->i2c);
}

static const struct compass_bus_ops compass_i2c_ops = {
	.kind = COMPASS_BUS_I2C,
	.transfer = compass_i2c_transfer,
	.detach = compass_i2c_detach,
};

int compass_i2c_attach(struct compass_bus *bus, compass_i2c_bus handle,
		       uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	i2c_device_config_t devcfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = scl_hz ? scl_hz : DEFAULT_SCL_HZ,
	};

	bus->ops = &compass_i2c_ops;
	bus->timeout_ms = timeout_ms;
	return compass_errno(i2c_master_bus_add_device(handle, &devcfg,
						       &bus->i2c));
}

/* ------------------------------------------------------------------ SPI -- */

/*
 * One full-duplex transaction, so CS stays low across cmd, tx and rx: clock
 * out cmd and tx followed by zeros, and keep what came back after them. The
 * buffers live on the stack, word-aligned: IDF bounces non-DMA-capable or
 * unaligned buffers through a copy of its own.
 */
static int compass_spi_transfer(struct compass_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	uint8_t out[COMPASS_SPI_MAX_LEN] __attribute__((aligned(4))) = { 0 };
	uint8_t in[COMPASS_SPI_MAX_LEN] __attribute__((aligned(4)));
	size_t n = 1 + tx_len + rx_len;
	spi_transaction_t t = {
		.length = n * 8,
		.tx_buffer = out,
		.rx_buffer = in,
	};
	int ret;

	if (tx_len > COMPASS_BUS_MAX_TX || n > sizeof(out))
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);
	ret = compass_errno(spi_device_polling_transmit(bus->spi, &t));
	if (!ret && rx_len)
		memcpy(rx, in + 1 + tx_len, rx_len);
	return ret;
}

static void compass_spi_detach(struct compass_bus *bus)
{
	spi_bus_remove_device(bus->spi);
}

static const struct compass_bus_ops compass_spi_ops = {
	.kind = COMPASS_BUS_SPI,
	.transfer = compass_spi_transfer,
	.detach = compass_spi_detach,
};

int compass_spi_attach(struct compass_bus *bus, spi_host_device_t host,
		       int cs_pin, uint32_t clock_hz, uint8_t mode,
		       uint32_t timeout_ms)
{
	spi_device_interface_config_t devcfg = {
		.mode = mode,
		.clock_speed_hz = (int)clock_hz,
		.spics_io_num = cs_pin,
		.queue_size = 1,
	};

	bus->ops = &compass_spi_ops;
	bus->timeout_ms = timeout_ms;
	return compass_errno(spi_bus_add_device(host, &devcfg, &bus->spi));
}

/* ----------------------------------------------------------------- UART -- */

/*
 * Drop whatever is waiting -- a late reply to an earlier command would read
 * as the start of this one -- then send, then wait for the whole reply.
 * uart_read_bytes() returns early only with everything; a short count means
 * the timeout ran out.
 */
static int compass_uart_transfer(struct compass_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint8_t out[1 + COMPASS_BUS_MAX_TX];
	int n, ret;

	if (tx_len > COMPASS_BUS_MAX_TX || rx_len > INT32_MAX)
		return -EINVAL;
	out[0] = cmd;
	if (tx_len)
		memcpy(out + 1, tx, tx_len);

	ret = compass_errno(uart_flush_input(bus->uart));
	if (ret)
		return ret;
	n = uart_write_bytes(bus->uart, out, 1 + tx_len);
	if (n != (int)(1 + tx_len))
		return -EIO;
	if (!rx_len)
		return 0;
	n = uart_read_bytes(bus->uart, rx, (uint32_t)rx_len,
			    compass_ticks(bus->timeout_ms));
	if (n < 0)
		return -EIO;
	return (size_t)n < rx_len ? -ETIMEDOUT : 0;
}

/* The driver is the app's: nothing was added, nothing to remove. */
static void compass_uart_detach(struct compass_bus *bus)
{
	(void)bus;
}

static const struct compass_bus_ops compass_uart_ops = {
	.kind = COMPASS_BUS_UART,
	.transfer = compass_uart_transfer,
	.detach = compass_uart_detach,
};

int compass_uart_attach(struct compass_bus *bus, uart_port_t port,
			uint32_t timeout_ms)
{
	if (!uart_is_driver_installed(port))
		return -EINVAL;
	bus->ops = &compass_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = port;
	return 0;
}

#endif /* ESP_PLATFORM */
