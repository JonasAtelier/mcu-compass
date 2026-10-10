/*
 * qmc5883p -- QST QMC5883P 3-axis magnetometer, as a compass chip
 * driver.
 *
 * What most GY-271 boards carry now, in place of the QMC5883L they are still
 * sold as. Different address (0x2C), different register map, a real chip ID,
 * four ranges up to +/-30 gauss, no temperature output and no DRDY pin.
 *
 * Samples come out of compass_read(). Everything below that is QMC5883P
 * specific takes the struct compass_dev that qmc5883p_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 *
 * Datasheet QST-PD-B002-22 Rev E is in docs/; every register value in
 * src/qmc5883p_regs.h and qmc5883p.c cites its section or table. Nothing here
 * has been run against a physical chip.
 */
#ifndef QMC5883P_H
#define QMC5883P_H

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

#define QMC5883P_ADDR	0x2C	/* 7-bit, fixed, §5.4 */

/* RNG, Table 18. The enum values are the register codes. */
enum qmc5883p_range {
	QMC5883P_RANGE_3000UT	= 0,	/* +/-30 G, 1000 LSB/G */
	QMC5883P_RANGE_1200UT	= 1,	/* +/-12 G, 2500 LSB/G */
	QMC5883P_RANGE_800UT	= 2,	/* +/-8 G, 3750 LSB/G; §7's choice */
	QMC5883P_RANGE_200UT	= 3,	/* +/-2 G, 15000 LSB/G -- the finest */
};

/* ODR, Table 17. Normal mode only: continuous mode runs flat out. */
enum qmc5883p_odr {
	QMC5883P_ODR_10HZ	= 0,
	QMC5883P_ODR_50HZ	= 1,
	QMC5883P_ODR_100HZ	= 2,
	QMC5883P_ODR_200HZ	= 3,
};

/* OSR1, Table 17: the internal filter. More is quieter and costs current. */
enum qmc5883p_osr1 {
	QMC5883P_OSR1_8		= 0,
	QMC5883P_OSR1_4		= 1,
	QMC5883P_OSR1_2		= 2,
	QMC5883P_OSR1_1		= 3,
};

/* OSR2, Table 17: a second, down-sampling filter on top of OSR1. */
enum qmc5883p_osr2 {
	QMC5883P_OSR2_1		= 0,
	QMC5883P_OSR2_2		= 1,
	QMC5883P_OSR2_4		= 2,
	QMC5883P_OSR2_8		= 3,
};

/*
 * All zero, or a NULL pointer, is the chip's own reset values in normal
 * mode: 400 kHz (ESP-IDF), +/-3000 uT, 10 Hz, OSR1 8, OSR2 1.
 */
struct qmc5883p_config {
#ifdef ESP_PLATFORM
	uint32_t scl_hz;		/* 0 selects 400 kHz, the part's maximum */
#endif
	enum qmc5883p_range range;
	enum qmc5883p_odr odr;
	enum qmc5883p_osr1 osr1;
	enum qmc5883p_osr2 osr2;
	/*
	 * false: normal mode, a measurement every 1/odr (§6.2.1).
	 * true: continuous mode, back to back with no sleep (§6.2.3); up to
	 * 1500 Hz and 2.2 mA (Table 2), and odr is ignored.
	 */
	bool continuous;
};

/*
 * bus is, on ESP-IDF, an i2c_master bus the app has already created; the
 * driver adds its device at cfg->scl_hz. On the STM32 HAL it is an I2C
 * handle the app has already set up with HAL_I2C_Init(), at 400 kHz or
 * less (§5.4: standard and fast mode only); the bus speed is the
 * handle's, not this driver's.
 *
 * Check the chip ID, soft reset, write the axis-sign register QST's setup
 * sequences start with, set range and mode. Fills compass->name, ops and
 * priv; leaves compass->mount alone.
 *
 * -EBUSY if compass is already bound (see struct compass_dev).
 * -ENXIO if nothing ACKs at 0x2C -- on a GY-271, try qmc5883l_probe(). Decided
 * before anything is allocated.
 * -ENODEV if CHIPID is not 0x80.
 * -EINVAL for a NULL compass or bus, or an enum out of range.
 * Bus failures come back as the errno of the failed transfer.
 */
int qmc5883p_probe(struct compass_dev *compass,
#ifdef ESP_PLATFORM
		   i2c_master_bus_handle_t bus,
#else
		   I2C_HandleTypeDef *bus,
#endif
		   const struct qmc5883p_config *cfg);

/* Puts the chip in suspend and releases it. Safe on an unprobed device. */
void qmc5883p_remove(struct compass_dev *compass);

/* One burst of all six data bytes, as counts, chip frame. */
int qmc5883p_read_raw(struct compass_dev *compass, int16_t field[3]);

/*
 * True once a new measurement is waiting. Reading STATUS clears the chip's
 * overflow flag too; the driver keeps it, so the next compass_read() still
 * reports -ERANGE.
 */
int qmc5883p_data_ready(struct compass_dev *compass, bool *ready);

int qmc5883p_set_range(struct compass_dev *compass, enum qmc5883p_range range);

/* Suspend (§6.2.4, 22 uA), or back to the configured mode. */
int qmc5883p_standby(struct compass_dev *compass, bool standby);

/*
 * §7.3 self-test: the field with and without the chip's internal stimulus,
 * as counts, with §7.3's signs -- (x1 - x2, y2 - y1, z2 - z1). Each axis
 * should move clearly away from zero.
 *
 * There is no pass/fail: the datasheet says to compare against a threshold
 * and never gives one. Both readings are taken at the +/-30 G range (1000
 * LSB/G), because §7.3's CTRL2 write forces it for the second; the driver
 * restores your range and mode afterwards. Takes a few milliseconds.
 * -ETIMEDOUT if no measurement arrives within 100 ms.
 *
 * The data registers still hold the stimulated reading until the next real
 * measurement; wait for qmc5883p_data_ready() before the next compass_read().
 * An overflow latched before the self-test and not yet reported by a
 * compass_read() is discarded along with the stimulus's own.
 */
int qmc5883p_self_test(struct compass_dev *compass, int16_t delta[3]);

/* Escape hatch for the registers this driver does not model. */
int qmc5883p_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		      size_t len);
int qmc5883p_reg_write(struct compass_dev *compass, uint8_t reg,
		       uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* QMC5883P_H */
