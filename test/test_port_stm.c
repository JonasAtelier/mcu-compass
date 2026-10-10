/*
 * Host-side check of compass_port_stm.c's bus ops against test/hal_stubs:
 * how each bus kind frames a transfer on the HAL, and HAL status -> errno.
 * The chip tests reach the I2C ops through real probes; this file drives all
 * three kinds directly, so the SPI chip select and the UART input drop are
 * pinned down even where no chip uses them yet.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass_priv.h"

/* Every HAL call, in order: one letter each, see the fakes below. */
static char calls[32];
static HAL_StatusTypeDef next_status;
static HAL_StatusTypeDef fail_on_second;	/* for the second SPI call */
static int spi_calls;

static uint8_t sent[32];	/* every byte transmitted, I2C/SPI/UART */
static size_t sent_len;
static uint8_t reply[32];	/* what a receive hands back */
static uint16_t last_mem_addr, last_dev_addr, last_size;

static I2C_HandleTypeDef i2c_handle;
static SPI_HandleTypeDef spi_handle;
static UART_HandleTypeDef uart_handle;
static GPIO_TypeDef cs_port;
static int stale_bytes;		/* bytes waiting in the UART data register */
static int timeout_after_send;	/* the UART receive after a send times out */
static int ore_cleared;

static void reset(void)
{
	calls[0] = '\0';
	next_status = fail_on_second = HAL_OK;
	spi_calls = 0;
	sent_len = 0;
	stale_bytes = ore_cleared = 0;
}

static void called(char c)
{
	size_t n = strlen(calls);

	assert(n + 1 < sizeof(calls));
	calls[n] = c;
	calls[n + 1] = '\0';
}

static void record(const uint8_t *p, uint16_t n)
{
	assert(sent_len + n <= sizeof(sent));
	memcpy(sent + sent_len, p, n);
	sent_len += n;
}

/* ---------------------------------------------------------------- I2C -- */

HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *h, uint16_t dev,
				   uint16_t mem, uint16_t mem_size,
				   uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && mem_size == I2C_MEMADD_SIZE_8BIT);
	called('r');
	last_dev_addr = dev;
	last_mem_addr = mem;
	last_size = size;
	if (next_status == HAL_OK)
		memcpy(data, reply, size);
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *h, uint16_t dev,
				    uint16_t mem, uint16_t mem_size,
				    uint8_t *data, uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle && mem_size == I2C_MEMADD_SIZE_8BIT);
	called('w');
	last_dev_addr = dev;
	last_mem_addr = mem;
	record(data, size);
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_Master_Transmit(I2C_HandleTypeDef *h, uint16_t dev,
					  uint8_t *data, uint16_t size,
					  uint32_t t)
{
	(void)t;
	assert(h == &i2c_handle);
	called('t');
	last_dev_addr = dev;
	record(data, size);
	return next_status;
}

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *h, uint16_t dev,
					uint32_t trials, uint32_t t)
{
	(void)t;
	(void)trials;
	assert(h == &i2c_handle);
	last_dev_addr = dev;
	return next_status;
}

/* ---------------------------------------------------------------- SPI -- */

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
	assert(port == &cs_port && pin == 0x0010);
	called(state == GPIO_PIN_RESET ? 'L' : 'H');
}

static HAL_StatusTypeDef spi_status(void)
{
	spi_calls++;
	return spi_calls == 2 && fail_on_second ? fail_on_second : next_status;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &spi_handle);
	called('T');
	record(data, size);
	return spi_status();
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *h, uint8_t *data,
				  uint16_t size, uint32_t t)
{
	HAL_StatusTypeDef status = spi_status();

	(void)t;
	assert(h == &spi_handle);
	called('R');
	if (status == HAL_OK)
		memcpy(data, reply, size);
	return status;
}

/* --------------------------------------------------------------- UART -- */

int stub_uart_get_flag(UART_HandleTypeDef *h, uint32_t flag)
{
	assert(h == &uart_handle && flag == UART_FLAG_RXNE);
	return stale_bytes > 0;
}

void stub_uart_clear_ore(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('O');
	ore_cleared++;
}

void stub_uart_flush(UART_HandleTypeDef *h)
{
	assert(h == &uart_handle);
	called('F');
	stale_bytes--;
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *data,
				    uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle);
	called('W');
	record(data, size);
	if (!timeout_after_send)
		return next_status;
	next_status = HAL_TIMEOUT;	/* for the receive that follows */
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t t)
{
	(void)t;
	assert(h == &uart_handle);
	called('R');
	last_size = size;
	if (next_status == HAL_OK)
		memcpy(data, reply, size);
	return next_status;
}

/* -------------------------------------------------------------- tests -- */

static void test_errno_mapping(void)
{
	static const struct {
		HAL_StatusTypeDef status;
		int expected;
	} map[] = {
		{ HAL_OK,	0 },
		{ HAL_ERROR,	-EIO },
		{ HAL_BUSY,	-EAGAIN },
		{ HAL_TIMEOUT,	-ETIMEDOUT },
	};
	struct compass_bus i2c, spi, uart;
	uint8_t b;

	compass_i2c_attach(&i2c, &i2c_handle, 0x1E, 0, 10);
	compass_spi_attach(&spi, &spi_handle, &cs_port, 0x0010, 10);
	compass_uart_attach(&uart, &uart_handle, 10);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		reset();
		next_status = map[i].status;
		assert(compass_reg_read(&i2c, 0, &b, 1) == map[i].expected);
		assert(compass_reg_read(&spi, 0, &b, 1) == map[i].expected);
		assert(compass_reg_read(&uart, 0, &b, 1) == map[i].expected);
	}
	/* Presence: a NACK is HAL_ERROR, which here means nothing there. */
	reset();
	next_status = HAL_ERROR;
	assert(compass_i2c_probe(&i2c_handle, 0x1E, 10) == -ENXIO);
	assert(last_dev_addr == 0x1E << 1);
}

static void test_i2c(void)
{
	static const uint8_t two[2] = { 0xAA, 0xBB };
	uint8_t rx[2] = { 0 }, many[COMPASS_BUS_MAX_TX + 1] = { 0 };
	struct compass_bus bus;

	assert(compass_i2c_attach(&bus, &i2c_handle, 0x1E, 0, 10) == 0);
	assert(bus.ops->kind == COMPASS_BUS_I2C);

	/* A read is Mem_Read: cmd, repeated start, burst. */
	reset();
	memcpy(reply, "\x01\x02", 2);
	assert(compass_reg_read(&bus, 0xA8, rx, 2) == 0);
	assert(strcmp(calls, "r") == 0);
	assert(last_dev_addr == 0x1E << 1 && last_mem_addr == 0xA8 &&
	       last_size == 2 && rx[0] == 1 && rx[1] == 2);

	/* A write is Mem_Write of the data; a bare cmd is one plain byte. */
	reset();
	assert(bus.ops->transfer(&bus, 0x20, two, 2, NULL, 0) == 0);
	assert(strcmp(calls, "w") == 0 && last_mem_addr == 0x20);
	assert(sent_len == 2 && sent[0] == 0xAA && sent[1] == 0xBB);
	reset();
	assert(bus.ops->transfer(&bus, 0x7E, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "t") == 0 && sent_len == 1 && sent[0] == 0x7E);

	/* What I2C here does not do, refused before the bus is touched. */
	reset();
	assert(bus.ops->transfer(&bus, 0x20, two, 1, rx, 1) == -EINVAL);
	assert(bus.ops->transfer(&bus, 0x20, many, sizeof(many), NULL, 0) ==
	       -EINVAL);
	assert(bus.ops->transfer(&bus, 0x20, NULL, 0, rx,
				 (size_t)UINT16_MAX + 1) == -EINVAL);
	assert(calls[0] == '\0');
	compass_bus_detach(&bus);
}

static void test_spi(void)
{
	static const uint8_t two[2] = { 0xAA, 0xBB };
	uint8_t rx[3] = { 0 };
	struct compass_bus bus;

	assert(compass_spi_attach(&bus, &spi_handle, &cs_port, 0x0010, 10) ==
	       0);
	assert(bus.ops->kind == COMPASS_BUS_SPI);

	/* CS low around cmd and the burst, high after. */
	reset();
	memcpy(reply, "\x01\x02\x03", 3);
	assert(compass_reg_read(&bus, 0xE8, rx, 3) == 0);
	assert(strcmp(calls, "LTRH") == 0);
	assert(sent_len == 1 && sent[0] == 0xE8);
	assert(rx[0] == 1 && rx[1] == 2 && rx[2] == 3);

	/* A write: cmd, then data, same window. */
	reset();
	assert(bus.ops->transfer(&bus, 0x20, two, 2, NULL, 0) == 0);
	assert(strcmp(calls, "LTTH") == 0);
	assert(sent_len == 3 && sent[0] == 0x20 && sent[1] == 0xAA &&
	       sent[2] == 0xBB);

	/* A failure stops the transfer, and CS still goes high. */
	reset();
	fail_on_second = HAL_TIMEOUT;
	assert(compass_reg_read(&bus, 0xE8, rx, 3) == -ETIMEDOUT);
	assert(strcmp(calls, "LTRH") == 0);
	reset();
	next_status = HAL_ERROR;
	assert(bus.ops->transfer(&bus, 0x20, two, 2, NULL, 0) == -EIO);
	assert(strcmp(calls, "LTH") == 0);
	compass_bus_detach(&bus);
}

static void test_uart(void)
{
	static const uint8_t args[2] = { 0x11, 0x22 };
	uint8_t rx[2] = { 0 };
	struct compass_bus bus;

	assert(compass_uart_attach(&bus, &uart_handle, 100) == 0);
	assert(bus.ops->kind == COMPASS_BUS_UART);

	/* Stale input dropped first: overrun cleared, two bytes read out. */
	reset();
	stale_bytes = 2;
	memcpy(reply, "\x12\x34", 2);
	assert(bus.ops->transfer(&bus, 0x13, args, 2, rx, 2) == 0);
	assert(strcmp(calls, "OFFWR") == 0);
	assert(stale_bytes == 0 && ore_cleared == 1);
	assert(sent_len == 3 && sent[0] == 0x13 && sent[1] == 0x11 &&
	       sent[2] == 0x22);
	assert(last_size == 2 && rx[0] == 0x12 && rx[1] == 0x34);

	/* No reply wanted: no receive. A failed send: no receive either. */
	reset();
	assert(bus.ops->transfer(&bus, 0x80, NULL, 0, NULL, 0) == 0);
	assert(strcmp(calls, "OW") == 0);
	reset();
	next_status = HAL_BUSY;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -EAGAIN);
	assert(strcmp(calls, "OW") == 0);
	compass_bus_detach(&bus);
}

/* A reply that stops short: the send passes, the receive times out. */
static void test_uart_short_reply(void)
{
	uint8_t rx[2];
	struct compass_bus bus;

	compass_uart_attach(&bus, &uart_handle, 100);
	reset();
	timeout_after_send = 1;
	assert(bus.ops->transfer(&bus, 0x13, NULL, 0, rx, 2) == -ETIMEDOUT);
	assert(strcmp(calls, "OWR") == 0);
	timeout_after_send = 0;
}

int main(void)
{
	test_errno_mapping();
	test_i2c();
	test_spi();
	test_uart();
	test_uart_short_reply();
	printf("test_port_stm: all passed\n");
	return 0;
}
