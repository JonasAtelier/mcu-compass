/*
 * Shared by the chip drivers inside this library. Not public API.
 *
 * The chip drivers are the same C on ESP-IDF and on the STM32 HAL; the bus,
 * the clock and the sleep under them are not. Those are the functions
 * declared below, defined once per platform in compass_port_esp.c and
 * compass_port_stm.c and bound at link time.
 *
 * ESP_PLATFORM, which ESP-IDF defines for every component, and the HAL's own
 * module switches are defined or not, never 0 or 1, so they are the one place
 * #ifdef is right; this library's CONFIG_COMPASS_* options are always tested
 * with #if.
 */
#ifndef COMPASS_PRIV_H
#define COMPASS_PRIV_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"

/* The bus argument of every probe on this platform. */
typedef i2c_master_bus_handle_t compass_i2c_bus;

/* One chip on the bus, as its driver holds it. */
struct compass_i2c {
	i2c_master_dev_handle_t dev;
};

#define compass_config_scl_hz(cfg)	((cfg)->scl_hz)

#else /* STM32 HAL */
#include CMSIS_device_header

#if !defined(HAL_I2C_MODULE_ENABLED) && \
    (CONFIG_COMPASS_QMC5883L || CONFIG_COMPASS_QMC5883P || \
     CONFIG_COMPASS_IST8310)
#error "the compass chip drivers need the HAL I2C module: #define HAL_I2C_MODULE_ENABLED in the app's stm32f{4,7}xx_hal_conf.h, with stm32f{4,7}xx_hal_i2c.c in the build"
#endif

#ifdef HAL_I2C_MODULE_ENABLED
typedef I2C_HandleTypeDef *compass_i2c_bus;

struct compass_i2c {
	I2C_HandleTypeDef *bus;
	uint8_t address;	/* 7-bit */
};
#endif

/* The configs have no scl_hz here: the bus speed is the handle's. */
#define compass_config_scl_hz(cfg)	0

#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Is anything at this 7-bit address? -ENXIO when nothing ACKs -- nothing at
 * that address -- not -ENODEV, which means something answered but it is the
 * wrong chip. A NACK in the middle of a transfer is a plain -EIO on both
 * platforms, which is why every probe decides presence here, never by its
 * first transfer.
 */
int compass_i2c_probe(compass_i2c_bus bus, uint8_t address,
		      uint32_t timeout_ms);

/*
 * Bind i2c to the chip at address. scl_hz 0 means 400 kHz. ESP-IDF sets the
 * clock per device; the HAL sets it per handle, so there scl_hz is ignored.
 * Undo with compass_i2c_detach().
 */
int compass_i2c_attach(struct compass_i2c *i2c, compass_i2c_bus bus,
		       uint8_t address, uint32_t scl_hz);
void compass_i2c_detach(struct compass_i2c *i2c);

/* Register address, repeated start, then len bytes. */
int compass_i2c_read(struct compass_i2c *i2c, uint8_t reg, void *buf,
		     size_t len, uint32_t timeout_ms);

/* Register address, then one data byte. */
int compass_i2c_write(struct compass_i2c *i2c, uint8_t reg, uint8_t value,
		      uint32_t timeout_ms);
#endif

/* Block for at least ms milliseconds: long, never short. */
void compass_sleep_ms(uint32_t ms);

/* A millisecond counter that wraps at 2^32, for compass_expired(). */
uint32_t compass_tick_ms(void);

/*
 * Has more than limit_ms passed since start, a compass_tick_ms() reading? The
 * first millisecond after start may be partial, so it takes strictly more
 * than limit_ms counted to be sure limit_ms really went by. Unsigned
 * subtraction, so it holds across the counter's wrap after 49.7 days.
 */
static inline int compass_expired(uint32_t start, uint32_t limit_ms)
{
	return (uint32_t)(compass_tick_ms() - start) > limit_ms;
}

#endif /* COMPASS_PRIV_H */
