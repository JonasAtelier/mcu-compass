/*
 * compass_priv.h on the STM32 HAL. stm-forge compiles every .c in src/, so
 * under ESP-IDF this file is empty; the ESP-IDF CMakeLists never lists it.
 */
#ifndef ESP_PLATFORM

#include "compass.h"
#include "compass_priv.h"

/*
 * The core is always built; each chip driver only under its own option. With
 * the library on and no chip picked, say so -- stm-forge compiles this file
 * only when PKG_compass = y, so this is the CMakeLists' "COMPASS on, no chip"
 * warning on the ESP-IDF side.
 */
#if !(CONFIG_COMPASS_QMC5883L || CONFIG_COMPASS_QMC5883P || \
      CONFIG_COMPASS_IST8310 || CONFIG_COMPASS_LIS3MDL || \
      CONFIG_COMPASS_CMPS14)
#warning "compass: no chip driver is selected; set COMPASS_<CHIP> = y under PKG_compass = y (or -DCONFIG_COMPASS_<CHIP>=1)"
#endif

/* Milliseconds dressed as microseconds; see compass.h for a real clock. */
__attribute__((weak)) int64_t compass_time_us(void)
{
	return (int64_t)HAL_GetTick() * 1000;
}

uint32_t compass_tick_ms(void)
{
	return HAL_GetTick();
}

/*
 * HAL_Delay() already adds one tick to what it is asked, because its first
 * tick may be about to fire, so ms is passed straight through: 10 ms waits
 * 10-11 ms at the HAL's default 1 kHz tick, and at a slower tick still never
 * less than 10. Long, never short -- as long as the app has not replaced the
 * weak HAL_Delay() with one that drops that tick. It busy-waits on SysTick,
 * so never call it from an interrupt at or above SysTick's priority.
 */
void compass_sleep_ms(uint32_t ms)
{
	HAL_Delay(ms);
}

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
/*
 * HAL_StatusTypeDef -> negative errno, at the bus boundary and nowhere else.
 * HAL_BUSY -- the bus or the handle in use, or a handle never initialised --
 * is -EAGAIN, because -EBUSY from a probe already means "this compass_dev is
 * bound". A NACK in the middle of a transfer is HAL_ERROR like any other
 * fault, so -EIO.
 */
static int compass_errno(HAL_StatusTypeDef status)
{
	switch (status) {
	case HAL_OK:
		return 0;
	case HAL_TIMEOUT:
		return -ETIMEDOUT;
	case HAL_BUSY:
		return -EAGAIN;
	default:
		return -EIO;
	}
}

/* Nothing to undo: the handle is the app's, and nothing was added to it. */
static void compass_hal_detach(struct compass_bus *bus)
{
	(void)bus;
}
#endif

#ifdef HAL_I2C_MODULE_ENABLED
/*
 * HAL_I2C_IsDeviceReady() returns HAL_ERROR when no trial was ACKed, which is
 * -ENXIO. A busy or stuck bus keeps its own errno. On the F4, no pull-ups or
 * SDA held low sets the peripheral's BUSY flag, so the HAL gives up after
 * 25 ms with HAL_BUSY, -EAGAIN; HAL_TIMEOUT, -ETIMEDOUT, means a START never
 * completed.
 */
int compass_i2c_probe(compass_i2c_bus handle, uint8_t address,
		      uint32_t timeout_ms)
{
	HAL_StatusTypeDef status;

	status = HAL_I2C_IsDeviceReady(handle, (uint16_t)(address << 1), 1,
				       timeout_ms);
	return status == HAL_ERROR ? -ENXIO : compass_errno(status);
}

/*
 * HAL_I2C_Mem_Read() is cmd, repeated start, burst; HAL_I2C_Mem_Write() is
 * cmd and the data in one write. The HAL refuses a Mem_Write of no data, so
 * a bare cmd goes out as a plain one-byte transmit.
 */
static int compass_i2c_transfer(struct compass_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	uint16_t address = (uint16_t)(bus->i2c.address << 1);

	if ((tx_len && rx_len) || tx_len > COMPASS_BUS_MAX_TX ||
	    rx_len > UINT16_MAX)
		return -EINVAL;
	if (rx_len)
		return compass_errno(HAL_I2C_Mem_Read(bus->i2c.handle, address,
						      cmd, I2C_MEMADD_SIZE_8BIT,
						      rx, (uint16_t)rx_len,
						      bus->timeout_ms));
	if (tx_len)
		return compass_errno(HAL_I2C_Mem_Write(bus->i2c.handle, address,
						       cmd, I2C_MEMADD_SIZE_8BIT,
						       (uint8_t *)tx,
						       (uint16_t)tx_len,
						       bus->timeout_ms));
	return compass_errno(HAL_I2C_Master_Transmit(bus->i2c.handle, address,
						     &cmd, 1,
						     bus->timeout_ms));
}

static const struct compass_bus_ops compass_i2c_ops = {
	.kind = COMPASS_BUS_I2C,
	.transfer = compass_i2c_transfer,
	.detach = compass_hal_detach,
};

int compass_i2c_attach(struct compass_bus *bus, compass_i2c_bus handle,
		       uint8_t address, uint32_t scl_hz, uint32_t timeout_ms)
{
	(void)scl_hz;
	bus->ops = &compass_i2c_ops;
	bus->timeout_ms = timeout_ms;
	bus->i2c.handle = handle;
	bus->i2c.address = address;
	return 0;
}
#endif /* HAL_I2C_MODULE_ENABLED */

#ifdef HAL_SPI_MODULE_ENABLED
/*
 * Chip select by hand, as the HAL's software NSS leaves it to the app: low
 * for the whole of cmd, tx and rx, high again whatever happened. The HAL's
 * transmit takes a non-const buffer in older releases, hence the casts; it
 * only reads it.
 */
static int compass_spi_transfer(struct compass_bus *bus, uint8_t cmd,
				const uint8_t *tx, size_t tx_len, uint8_t *rx,
				size_t rx_len)
{
	SPI_HandleTypeDef *handle = bus->spi.handle;
	HAL_StatusTypeDef status;

	if (tx_len > COMPASS_BUS_MAX_TX || rx_len > UINT16_MAX)
		return -EINVAL;

	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_RESET);
	status = HAL_SPI_Transmit(handle, &cmd, 1, bus->timeout_ms);
	if (status == HAL_OK && tx_len)
		status = HAL_SPI_Transmit(handle, (uint8_t *)tx,
					  (uint16_t)tx_len, bus->timeout_ms);
	if (status == HAL_OK && rx_len)
		status = HAL_SPI_Receive(handle, rx, (uint16_t)rx_len,
					 bus->timeout_ms);
	HAL_GPIO_WritePin(bus->spi.cs_port, bus->spi.cs_pin, GPIO_PIN_SET);
	return compass_errno(status);
}

static const struct compass_bus_ops compass_spi_ops = {
	.kind = COMPASS_BUS_SPI,
	.transfer = compass_spi_transfer,
	.detach = compass_hal_detach,
};

int compass_spi_attach(struct compass_bus *bus, SPI_HandleTypeDef *handle,
		       GPIO_TypeDef *cs_port, uint16_t cs_pin,
		       uint32_t timeout_ms)
{
	bus->ops = &compass_spi_ops;
	bus->timeout_ms = timeout_ms;
	bus->spi.handle = handle;
	bus->spi.cs_port = cs_port;
	bus->spi.cs_pin = cs_pin;
	return 0;
}
#endif /* HAL_SPI_MODULE_ENABLED */

#ifdef HAL_UART_MODULE_ENABLED
/*
 * Blocking HAL_UART_Receive() hands back whatever already sits in the data
 * register, so a late byte from an earlier exchange would be read as the
 * start of this reply. Drop it first: clear an overrun, then read the data
 * register empty (on the F7 that macro is a flush request). Bounded by the
 * timeout, in case the line never goes quiet.
 */
static void compass_uart_drop_input(struct compass_bus *bus)
{
	UART_HandleTypeDef *handle = bus->uart;
	uint32_t start = compass_tick_ms();

	__HAL_UART_CLEAR_OREFLAG(handle);
	while (__HAL_UART_GET_FLAG(handle, UART_FLAG_RXNE) &&
	       !compass_expired(start, bus->timeout_ms))
		__HAL_UART_FLUSH_DRREGISTER(handle);
}

static int compass_uart_transfer(struct compass_bus *bus, uint8_t cmd,
				 const uint8_t *tx, size_t tx_len, uint8_t *rx,
				 size_t rx_len)
{
	uint8_t out[1 + COMPASS_BUS_MAX_TX];
	int ret;

	if (tx_len > COMPASS_BUS_MAX_TX || rx_len > UINT16_MAX)
		return -EINVAL;

	out[0] = cmd;
	for (size_t i = 0; i < tx_len; i++)
		out[1 + i] = tx[i];

	compass_uart_drop_input(bus);
	ret = compass_errno(HAL_UART_Transmit(bus->uart, out,
					      (uint16_t)(1 + tx_len),
					      bus->timeout_ms));
	if (ret || !rx_len)
		return ret;
	/* A reply that stops short runs into the timeout: HAL_TIMEOUT. */
	return compass_errno(HAL_UART_Receive(bus->uart, rx, (uint16_t)rx_len,
					      bus->timeout_ms));
}

static const struct compass_bus_ops compass_uart_ops = {
	.kind = COMPASS_BUS_UART,
	.transfer = compass_uart_transfer,
	.detach = compass_hal_detach,
};

int compass_uart_attach(struct compass_bus *bus, UART_HandleTypeDef *handle,
			uint32_t timeout_ms)
{
	bus->ops = &compass_uart_ops;
	bus->timeout_ms = timeout_ms;
	bus->uart = handle;
	return 0;
}
#endif /* HAL_UART_MODULE_ENABLED */

#endif /* !ESP_PLATFORM */
