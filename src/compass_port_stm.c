/*
 * compass_priv.h on the STM32 HAL. stm-forge compiles every .c in src/, so under
 * ESP-IDF this file is empty; the ESP-IDF CMakeLists never lists it.
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
      CONFIG_COMPASS_IST8310)
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

#ifdef HAL_I2C_MODULE_ENABLED
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

/*
 * HAL_I2C_IsDeviceReady() returns HAL_ERROR when no trial was ACKed, which is
 * -ENXIO. A busy or stuck bus keeps its own errno. On the F4, no pull-ups or
 * SDA held low sets the peripheral's BUSY flag, so the HAL gives up after
 * 25 ms with HAL_BUSY, -EAGAIN; HAL_TIMEOUT, -ETIMEDOUT, means a START never
 * completed.
 */
int compass_i2c_probe(compass_i2c_bus bus, uint8_t address,
		      uint32_t timeout_ms)
{
	HAL_StatusTypeDef status;

	status = HAL_I2C_IsDeviceReady(bus, (uint16_t)(address << 1), 1,
				       timeout_ms);
	return status == HAL_ERROR ? -ENXIO : compass_errno(status);
}

int compass_i2c_attach(struct compass_i2c *i2c, compass_i2c_bus bus,
		       uint8_t address, uint32_t scl_hz)
{
	(void)scl_hz;
	i2c->bus = bus;
	i2c->address = address;
	return 0;
}

void compass_i2c_detach(struct compass_i2c *i2c)
{
	(void)i2c;
}

/* HAL_I2C_Mem_Read(): register address, repeated start, burst. */
int compass_i2c_read(struct compass_i2c *i2c, uint8_t reg, void *buf,
		     size_t len, uint32_t timeout_ms)
{
	if (len > UINT16_MAX)
		return -EINVAL;
	return compass_errno(HAL_I2C_Mem_Read(i2c->bus,
					      (uint16_t)(i2c->address << 1),
					      reg, I2C_MEMADD_SIZE_8BIT, buf,
					      (uint16_t)len, timeout_ms));
}

int compass_i2c_write(struct compass_i2c *i2c, uint8_t reg, uint8_t value,
		      uint32_t timeout_ms)
{
	return compass_errno(HAL_I2C_Mem_Write(i2c->bus,
					       (uint16_t)(i2c->address << 1),
					       reg, I2C_MEMADD_SIZE_8BIT, &value,
					       1, timeout_ms));
}
#endif /* HAL_I2C_MODULE_ENABLED */

#endif /* !ESP_PLATFORM */
