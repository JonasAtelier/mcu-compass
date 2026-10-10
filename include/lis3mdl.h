/*
 * lis3mdl -- ST LIS3MDL 3-axis magnetometer, as a compass chip driver, on
 * I2C or on 4-wire SPI.
 *
 * Four ranges, +/-4 to +/-16 gauss, up to 80 Hz, a relative die temperature,
 * and a WHO_AM_I to tell it apart. The same chip logic runs on either bus;
 * pick one probe. CS high selects I2C (§5), so on I2C tie it to Vdd_IO.
 *
 * Samples come out of compass_read(). Everything below that is LIS3MDL
 * specific takes the struct compass_dev that a probe filled, and returns
 * -EINVAL if handed a device some other driver probed.
 *
 * Datasheet DocID024204 Rev 4 is in docs/; every register value in
 * src/lis3mdl_regs.h and lis3mdl.c cites its section or table. Nothing here
 * has been run against a physical chip.
 */
#ifndef LIS3MDL_H
#define LIS3MDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#else
#include CMSIS_device_header
#endif

#include "compass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* §5.1.1, Table 11: SAD 00111x0b, x = SDO/SA1, to GND or to Vdd_IO. */
#define LIS3MDL_ADDR_LOW	0x1C
#define LIS3MDL_ADDR_HIGH	0x1E

/* Table 25. The earth's field, 25-65 uT, sits well inside the finest. */
enum lis3mdl_range {
	LIS3MDL_RANGE_400UT	= 0,	/* +/-4 gauss, 6842 LSB/G */
	LIS3MDL_RANGE_800UT	= 1,	/* +/-8 gauss, 3421 LSB/G */
	LIS3MDL_RANGE_1200UT	= 2,	/* +/-12 gauss, 2281 LSB/G */
	LIS3MDL_RANGE_1600UT	= 3,	/* +/-16 gauss, 1711 LSB/G */
};

/*
 * Table 22's rates. Not the register codes: 10 Hz, the chip's own default,
 * is 0 here so that a zeroed config keeps it.
 */
enum lis3mdl_odr {
	LIS3MDL_ODR_10HZ	= 0,
	LIS3MDL_ODR_0_625HZ,
	LIS3MDL_ODR_1_25HZ,
	LIS3MDL_ODR_2_5HZ,
	LIS3MDL_ODR_5HZ,
	LIS3MDL_ODR_20HZ,
	LIS3MDL_ODR_40HZ,
	LIS3MDL_ODR_80HZ,
};

/*
 * Tables 21 and 31: the operating mode, set on X/Y and Z alike. Higher
 * performance is quieter and draws more: 40 uA in low-power and 270 uA in
 * ultra-high at 20 Hz (Table 5).
 */
enum lis3mdl_performance {
	LIS3MDL_LOW_POWER	= 0,
	LIS3MDL_MEDIUM		= 1,
	LIS3MDL_HIGH		= 2,
	LIS3MDL_ULTRA_HIGH	= 3,
};

/*
 * All zero, or a NULL pointer: address 0x1E, 400 kHz on ESP-IDF I2C, and the
 * chip's own reset values -- +/-4 gauss, 10 Hz, low-power. The SPI probe
 * ignores address and scl_hz.
 */
struct lis3mdl_config {
#ifdef ESP_PLATFORM
	uint32_t scl_hz;		/* 0 selects 400 kHz (Table 7) */
#endif
	uint8_t address;		/* 0x1C or 0x1E; 0 means 0x1E */
	enum lis3mdl_range range;
	enum lis3mdl_odr odr;
	enum lis3mdl_performance performance;
};

/*
 * Where the chip sits on SPI. The bus itself is the app's: on ESP-IDF a host
 * already spi_bus_initialize()d, to which the probe adds the chip; on the
 * STM32 HAL a handle already HAL_SPI_Init()ed in mode 3 (SPC idles high and
 * data is captured on the rising edge, §5.2), 8-bit frames, MSB first, at
 * 10 MHz or less (Table 6).
 */
struct lis3mdl_spi_config {
#ifdef ESP_PLATFORM
	spi_host_device_t host;
	int cs_pin;
	uint32_t clock_hz;		/* 0 selects 5 MHz; at most 10 MHz */
#else
	SPI_HandleTypeDef *bus;
	/* Software chip select: a GPIO output the app set up, idle high. */
	GPIO_TypeDef *cs_port;
	uint16_t cs_pin;
#endif
};

/*
 * Probe on I2C. bus is, on ESP-IDF, an i2c_master bus the app has already
 * created; the driver adds its device at cfg->scl_hz. On the STM32 HAL it is
 * an I2C handle the app has already set up with HAL_I2C_Init(), at 400 kHz or
 * less (Table 7); the bus speed is the handle's.
 *
 * Check WHO_AM_I, write every control register, start continuous
 * conversion. Fills compass->name, ops and priv; leaves compass->mount
 * alone.
 *
 * -EBUSY if compass is already bound (see struct compass_dev).
 * -ENXIO if nothing ACKs at the address, decided before anything is
 * allocated.
 * -ENODEV if WHO_AM_I is not 0x3D.
 * -EINVAL for a NULL compass or bus, an address other than 0x1C or 0x1E, or
 * an enum out of range.
 * Bus failures come back as the errno of the failed transfer.
 */
int lis3mdl_probe_i2c(struct compass_dev *compass,
#ifdef ESP_PLATFORM
		      i2c_master_bus_handle_t bus,
#else
		      I2C_HandleTypeDef *bus,
#endif
		      const struct lis3mdl_config *cfg);

/*
 * Probe on SPI, as above. SPI has no acknowledge, so an empty bus is not
 * -ENXIO: WHO_AM_I reads 0x00 or 0xFF and the probe returns -ENODEV.
 * -EINVAL also for a NULL spi, or on ESP-IDF a clock above 10 MHz.
 */
int lis3mdl_probe_spi(struct compass_dev *compass,
		      const struct lis3mdl_spi_config *spi,
		      const struct lis3mdl_config *cfg);

/* Powers the chip down and releases it. Safe on an unprobed device. */
void lis3mdl_remove(struct compass_dev *compass);

/* One burst of all six data bytes, as counts, chip frame. */
int lis3mdl_read_raw(struct compass_dev *compass, int16_t field[3]);

/* True once a new X, Y and Z set is waiting. */
int lis3mdl_data_ready(struct compass_dev *compass, bool *ready);

/*
 * Die temperature, in degrees relative to an uncalibrated zero: 8 LSB per
 * degree (Table 4), and the datasheet gives no zero point. Useful for drift
 * compensation, useless as a thermometer.
 */
int lis3mdl_temperature(struct compass_dev *compass, float *relative_c);

int lis3mdl_set_range(struct compass_dev *compass, enum lis3mdl_range range);

/* Power-down (Table 28), or back to continuous conversion. */
int lis3mdl_standby(struct compass_dev *compass, bool standby);

/*
 * Escape hatch for the registers this driver does not model. reg is the bare
 * address; the driver adds the bus's read and auto-increment bits.
 */
int lis3mdl_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		     size_t len);
int lis3mdl_reg_write(struct compass_dev *compass, uint8_t reg,
		      uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* LIS3MDL_H */
