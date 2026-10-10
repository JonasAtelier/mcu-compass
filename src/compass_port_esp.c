/*
 * compass_priv.h on ESP-IDF. The CMakeLists lists this file; stm-forge
 * compiles every .c in src/, so outside ESP-IDF it is empty.
 */
#ifdef ESP_PLATFORM

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_priv.h"

/* Fast mode, the most the parts here take (QMC5883P §5.4, IST8310 §6.1). */
#define DEFAULT_SCL_HZ		400000

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
void compass_sleep_ms(uint32_t ms)
{
	vTaskDelay((TickType_t)(((uint64_t)ms * configTICK_RATE_HZ + 999) /
				1000) + 1);
}

int compass_i2c_probe(compass_i2c_bus bus, uint8_t address,
		      uint32_t timeout_ms)
{
	return compass_errno(i2c_master_probe(bus, address, (int)timeout_ms));
}

int compass_i2c_attach(struct compass_i2c *i2c, compass_i2c_bus bus,
		       uint8_t address, uint32_t scl_hz)
{
	i2c_device_config_t devcfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = scl_hz ? scl_hz : DEFAULT_SCL_HZ,
	};

	return compass_errno(i2c_master_bus_add_device(bus, &devcfg,
						       &i2c->dev));
}

void compass_i2c_detach(struct compass_i2c *i2c)
{
	i2c_master_bus_rm_device(i2c->dev);
}

int compass_i2c_read(struct compass_i2c *i2c, uint8_t reg, void *buf,
		     size_t len, uint32_t timeout_ms)
{
	return compass_errno(i2c_master_transmit_receive(i2c->dev, &reg, 1, buf,
							 len,
							 (int)timeout_ms));
}

int compass_i2c_write(struct compass_i2c *i2c, uint8_t reg, uint8_t value,
		      uint32_t timeout_ms)
{
	uint8_t b[2] = { reg, value };

	return compass_errno(i2c_master_transmit(i2c->dev, b, sizeof(b),
						 (int)timeout_ms));
}

#endif /* ESP_PLATFORM */
