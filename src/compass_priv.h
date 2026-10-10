/*
 * Shared by the chip drivers inside this library. Not public API.
 *
 * Two layers sit under the chip drivers, the way regmap sits under a kernel
 * driver:
 *
 *   - The bus kind is chosen at run time. A chip holds one struct compass_bus
 *     and talks to it only through bus->ops->transfer(), so one driver can
 *     sit on I2C or on SPI, and a command/response part on a UART.
 *   - The platform is chosen at link time. compass_port_esp.c and
 *     compass_port_stm.c each define the ops tables, the attach functions,
 *     the clock and the sleep below for their platform; the other one
 *     compiles to nothing.
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

/* The most bytes a transfer() sends after cmd. Writes here are one byte. */
#define COMPASS_BUS_MAX_TX	16

struct compass_bus;

enum compass_bus_kind {
	COMPASS_BUS_I2C,
	COMPASS_BUS_SPI,
	COMPASS_BUS_UART,
};

struct compass_bus_ops {
	enum compass_bus_kind kind;
	/*
	 * One transaction: cmd, then tx_len bytes of tx, then rx_len bytes
	 * into rx. cmd is a register address on I2C and SPI -- with any SPI
	 * read or auto-increment bits already set by the chip -- and a
	 * command byte on a UART.
	 *
	 *   I2C:  rx_len ? cmd, repeated start, read : cmd and tx, one write.
	 *         Sending tx and then reading is -EINVAL.
	 *   SPI:  CS low, cmd, tx, rx, CS high.
	 *   UART: drop stale input, send cmd and tx, then wait for rx_len
	 *         bytes; fewer within the timeout is -ETIMEDOUT.
	 *
	 * tx_len above COMPASS_BUS_MAX_TX is -EINVAL. 0 or a negative errno.
	 */
	int (*transfer)(struct compass_bus *bus, uint8_t cmd, const uint8_t *tx,
			size_t tx_len, uint8_t *rx, size_t rx_len);
	/* Undo the attach. The bus itself is the app's and is left alone. */
	void (*detach)(struct compass_bus *bus);
};

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/uart.h"

/* The I2C argument of every I2C probe on this platform, and the UART one. */
typedef i2c_master_bus_handle_t compass_i2c_bus;
typedef uart_port_t compass_uart_port;

/*
 * One chip on one bus, as its driver holds it. COMPASS_HAVE_BUS says the
 * platform has at least one bus kind, so the struct exists.
 */
#define COMPASS_HAVE_BUS
struct compass_bus {
	const struct compass_bus_ops *ops;
	uint32_t timeout_ms;
	union {
		i2c_master_dev_handle_t i2c;
		spi_device_handle_t spi;
		uart_port_t uart;
	};
};

#define compass_config_scl_hz(cfg)	((cfg)->scl_hz)

#else /* STM32 HAL */
#include CMSIS_device_header

#if !defined(HAL_I2C_MODULE_ENABLED) && \
    (CONFIG_COMPASS_QMC5883L || CONFIG_COMPASS_QMC5883P || \
     CONFIG_COMPASS_IST8310 || CONFIG_COMPASS_LIS3MDL)
#error "the compass chip drivers need the HAL I2C module: #define HAL_I2C_MODULE_ENABLED in the app's stm32f{4,7}xx_hal_conf.h, with stm32f{4,7}xx_hal_i2c.c in the build"
#endif
#if !defined(HAL_SPI_MODULE_ENABLED) && CONFIG_COMPASS_LIS3MDL
#error "COMPASS_LIS3MDL needs the HAL SPI module: #define HAL_SPI_MODULE_ENABLED in the app's stm32f{4,7}xx_hal_conf.h, with stm32f{4,7}xx_hal_spi.c in the build"
#endif
#if !defined(HAL_UART_MODULE_ENABLED) && CONFIG_COMPASS_CMPS14
#error "COMPASS_CMPS14 needs the HAL UART module: #define HAL_UART_MODULE_ENABLED in the app's stm32f{4,7}xx_hal_conf.h, with stm32f{4,7}xx_hal_uart.c in the build"
#endif

#ifdef HAL_I2C_MODULE_ENABLED
typedef I2C_HandleTypeDef *compass_i2c_bus;
#endif
#ifdef HAL_UART_MODULE_ENABLED
typedef UART_HandleTypeDef *compass_uart_port;
#endif

#if defined(HAL_I2C_MODULE_ENABLED) || defined(HAL_SPI_MODULE_ENABLED) || \
    defined(HAL_UART_MODULE_ENABLED)
#define COMPASS_HAVE_BUS
struct compass_bus {
	const struct compass_bus_ops *ops;
	uint32_t timeout_ms;
	union {
#ifdef HAL_I2C_MODULE_ENABLED
		struct {
			I2C_HandleTypeDef *handle;
			uint8_t address;	/* 7-bit */
		} i2c;
#endif
#ifdef HAL_SPI_MODULE_ENABLED
		struct {
			SPI_HandleTypeDef *handle;
			GPIO_TypeDef *cs_port;	/* app's output, idle high */
			uint16_t cs_pin;
		} spi;
#endif
#ifdef HAL_UART_MODULE_ENABLED
		UART_HandleTypeDef *uart;
#endif
	};
};
#endif

/* The configs have no scl_hz here: the bus speed is the handle's. */
#define compass_config_scl_hz(cfg)	0

#endif

#ifdef COMPASS_HAVE_BUS
/* Register helpers on top of transfer(), for the chips that have registers. */
static inline int compass_reg_read(struct compass_bus *bus, uint8_t reg,
				   void *buf, size_t len)
{
	return bus->ops->transfer(bus, reg, NULL, 0, buf, len);
}

static inline int compass_reg_write(struct compass_bus *bus, uint8_t reg,
				    uint8_t value)
{
	return bus->ops->transfer(bus, reg, &value, 1, NULL, 0);
}

static inline void compass_bus_detach(struct compass_bus *bus)
{
	bus->ops->detach(bus);
}
#endif

#if defined(ESP_PLATFORM) || defined(HAL_I2C_MODULE_ENABLED)
/*
 * Is anything at this 7-bit address? -ENXIO when nothing ACKs -- nothing at
 * that address -- not -ENODEV, which means something answered but it is the
 * wrong chip. A NACK in the middle of a transfer is a plain -EIO on both
 * platforms, which is why every I2C probe decides presence here, never by
 * its first transfer.
 */
int compass_i2c_probe(compass_i2c_bus handle, uint8_t address,
		      uint32_t timeout_ms);

/*
 * Bind bus to the I2C chip at address. scl_hz 0 means 400 kHz. ESP-IDF sets
 * the clock per device; the HAL sets it per handle, so there scl_hz is
 * ignored. Undo with compass_bus_detach().
 */
int compass_i2c_attach(struct compass_bus *bus, compass_i2c_bus handle,
		       uint8_t address, uint32_t scl_hz, uint32_t timeout_ms);
#endif

#ifdef ESP_PLATFORM
/*
 * Add the SPI chip on host's cs_pin, in SPI mode 0-3 at clock_hz. The host is
 * the app's, already spi_bus_initialize()d.
 */
int compass_spi_attach(struct compass_bus *bus, spi_host_device_t host,
		       int cs_pin, uint32_t clock_hz, uint8_t mode,
		       uint32_t timeout_ms);

/* port's driver is the app's, already installed and configured. */
int compass_uart_attach(struct compass_bus *bus, uart_port_t port,
			uint32_t timeout_ms);
#else
#ifdef HAL_SPI_MODULE_ENABLED
/*
 * Bind bus to the SPI chip behind cs_port/cs_pin. Mode, clock and frame are
 * the handle's, and the chip select is an output the app has set up, idle
 * high.
 */
int compass_spi_attach(struct compass_bus *bus, SPI_HandleTypeDef *handle,
		       GPIO_TypeDef *cs_port, uint16_t cs_pin,
		       uint32_t timeout_ms);
#endif
#ifdef HAL_UART_MODULE_ENABLED
/* handle is the app's, already HAL_UART_Init()ed. */
int compass_uart_attach(struct compass_bus *bus, UART_HandleTypeDef *handle,
			uint32_t timeout_ms);
#endif
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
