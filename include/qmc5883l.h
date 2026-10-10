/*
 * qmc5883l -- QST QMC5883L 3-axis magnetometer, as a compass chip
 * driver.
 *
 * The compass on older GY-271 boards, and the one hiding behind the IMU on a
 * GY-86. Three field components, plus a relative die temperature.
 *
 * NOT an HMC5883L (Honeywell, 0x1E) and NOT a QMC5883P (QST, 0x2C) -- three
 * different register maps behind boards sold under the same names. Newer
 * GY-271s mostly carry the P; see the README and the gy271 example.
 *
 * Samples come out of compass_read(). Everything below that is QMC5883L
 * specific takes the struct compass_dev that qmc5883l_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 *
 * Register provenance is in src/qmc5883l_regs.h. Nothing here has been run
 * against a physical chip.
 */
#ifndef QMC5883L_H
#define QMC5883L_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/i2c_master.h"
#else
#include CMSIS_device_header
#endif

#include "compass.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QMC5883L_ADDR	0x0D	/* 7-bit, fixed; the part has no address pin */

enum qmc5883l_range {
	QMC5883L_RANGE_200UT	= 0,	/* +/-2 gauss, 12000 LSB/G -- the finer one */
	QMC5883L_RANGE_800UT	= 1,	/* +/-8 gauss, 3000 LSB/G */
};

enum qmc5883l_odr {
	QMC5883L_ODR_10HZ	= 0,
	QMC5883L_ODR_50HZ	= 1,
	QMC5883L_ODR_100HZ	= 2,
	QMC5883L_ODR_200HZ	= 3,
};

enum qmc5883l_osr {
	QMC5883L_OSR_512	= 0,	/* quietest, most current */
	QMC5883L_OSR_256	= 1,
	QMC5883L_OSR_128	= 2,
	QMC5883L_OSR_64		= 3,
};

/* All zero, or a NULL pointer: 400 kHz (ESP-IDF), +/-200 uT, 10 Hz, 512x. */
struct qmc5883l_config {
#ifdef ESP_PLATFORM
	uint32_t scl_hz;		/* 0 selects 400 kHz */
#endif
	enum qmc5883l_range range;
	enum qmc5883l_odr odr;
	enum qmc5883l_osr osr;
};

/*
 * bus is, on ESP-IDF, an i2c_master bus the app has already created; the
 * driver adds its device at cfg->scl_hz. On the STM32 HAL it is an I2C
 * handle the app has already set up with HAL_I2C_Init(), at 400 kHz or
 * less; the bus speed is the handle's, not this driver's.
 *
 * Soft reset the device, prove something is actually there, then configure
 * it and start continuous measurement. Fills compass->name, ops and priv;
 * leaves compass->mount alone.
 *
 * -EBUSY if compass is already bound (see struct compass_dev).
 * -ENXIO if nothing ACKs at 0x0D, decided before anything is allocated.
 * -ENODEV if what answers does not behave like a QMC5883L. The identity
 * register reads 0xFF, which a bare bus also reads, so this additionally
 * writes and reads back a register.
 * -EINVAL for a NULL compass or bus, or an enum out of range.
 * Bus failures come back as the errno of the failed transfer.
 */
int qmc5883l_probe(struct compass_dev *compass,
#ifdef ESP_PLATFORM
		   i2c_master_bus_handle_t bus,
#else
		   I2C_HandleTypeDef *bus,
#endif
		   const struct qmc5883l_config *cfg);

/* Puts the chip in standby and releases it. Safe on an unprobed device. */
void qmc5883l_remove(struct compass_dev *compass);

/* One burst of all six data bytes, as counts, chip frame. */
int qmc5883l_read_raw(struct compass_dev *compass, int16_t field[3]);

/* True once a new measurement is waiting. */
int qmc5883l_data_ready(struct compass_dev *compass, bool *ready);

/*
 * Die temperature, in degrees relative to an uncalibrated zero. The part does
 * not know its absolute temperature -- only how far it has moved. Useful for
 * compensating drift, useless as a thermometer.
 */
int qmc5883l_temperature(struct compass_dev *compass, float *relative_c);

int qmc5883l_set_range(struct compass_dev *compass, enum qmc5883l_range range);
int qmc5883l_standby(struct compass_dev *compass, bool standby);

/* Escape hatch for the registers this driver does not model. */
int qmc5883l_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		      size_t len);
int qmc5883l_reg_write(struct compass_dev *compass, uint8_t reg,
		       uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* QMC5883L_H */
