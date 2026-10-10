/*
 * ist8310 -- iSentek IST8310 3-axis magnetometer, as a compass chip
 * driver.
 *
 * The compass on many drone GPS masts and flight controllers. AMR, +/-1600 uT
 * on X and Y and +/-2500 uT on Z, 0.3 uT per count, up to 200 Hz. Four
 * addresses by its CAD pins.
 *
 * There is no continuous mode: every measurement is triggered. The driver
 * starts one at probe and another each time compass_read() collects a
 * result, so reads never wait -- and a read that comes before the next result
 * is -ENODATA. Read at 100 Hz or slower (§3.1.1: 6 ms per measurement with
 * 16x averaging) and every read gets a fresh sample.
 *
 * Samples come out of compass_read(). Everything below that is IST8310
 * specific takes the struct compass_dev that ist8310_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed.
 *
 * Datasheet Version 1.2 is in docs/; every register value in
 * src/ist8310/ist8310_regs.h and ist8310.c cites its section. Nothing here has
 * been run against a physical chip.
 */
#ifndef IST8310_H
#define IST8310_H

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

/* 7-bit, by CAD1 and CAD0 (§6.1.1). Both floating is 0x0E. */
#define IST8310_ADDR_LL		0x0C	/* CAD1 VSS, CAD0 VSS */
#define IST8310_ADDR_LH		0x0D	/* CAD1 VSS, CAD0 VDD */
#define IST8310_ADDR_HL		0x0E	/* CAD1 VDD, CAD0 VSS; also floating */
#define IST8310_ADDR_HH		0x0F	/* CAD1 VDD, CAD0 VDD */

/* AVGCNTL, §6.4.10. The enum values are the register codes. */
enum ist8310_avg {
	IST8310_AVG_1	= 0,	/* no averaging */
	IST8310_AVG_2	= 1,
	IST8310_AVG_4	= 2,
	IST8310_AVG_8	= 3,
	IST8310_AVG_16	= 4,	/* §3.1.1's low-noise choice */
};

/*
 * All zero, or a NULL pointer: address 0x0E, 400 kHz (ESP-IDF), no
 * averaging. §3.1.1 recommends 16 on both for low noise.
 */
struct ist8310_config {
#ifdef ESP_PLATFORM
	uint32_t scl_hz;	/* 0 selects 400 kHz, the part's maximum (§6.1) */
#endif
	uint8_t address;	/* 0x0C-0x0F; 0 means 0x0E */
	enum ist8310_avg avg_xz;
	enum ist8310_avg avg_y;
};

/*
 * bus is, on ESP-IDF, an i2c_master bus the app has already created; the
 * driver adds its device at cfg->scl_hz. On the STM32 HAL it is an I2C
 * handle the app has already set up with HAL_I2C_Init(), at 400 kHz or
 * less (§6.1); the bus speed is the handle's, not this driver's.
 *
 * Check WAI, soft reset, set averaging and §3.1.1's pulse duration, start
 * the first measurement. Fills compass->name, ops and priv; leaves
 * compass->mount alone. Waits 50-51 ms for the reset.
 *
 * -EBUSY if compass is already bound (see struct compass_dev).
 * -ENXIO if nothing ACKs at the address, decided before anything is
 * allocated.
 * -ENODEV if WAI is not 0x10.
 * -EINVAL for a NULL compass or bus, an address outside 0x0C-0x0F, or an
 * enum out of range.
 * Bus failures come back as the errno of the failed transfer.
 */
int ist8310_probe(struct compass_dev *compass,
#ifdef ESP_PLATFORM
		  i2c_master_bus_handle_t bus,
#else
		  I2C_HandleTypeDef *bus,
#endif
		  const struct ist8310_config *cfg);

/* Puts the chip in stand-by and releases it. Safe on an unprobed device. */
void ist8310_remove(struct compass_dev *compass);

/*
 * The measurement compass_read() would take, as counts, chip frame, and the
 * next one started. Same -ENODATA rule; no range check.
 */
int ist8310_read_raw(struct compass_dev *compass, int16_t field[3]);

/* True once a measurement is waiting. Reading STAT1 clears nothing (§3.4). */
int ist8310_data_ready(struct compass_dev *compass, bool *ready);

/*
 * Escape hatch for the registers this driver does not model. Reading the
 * data registers here takes the waiting result (§3.4); compass_read() then
 * sees nothing ready and starts another measurement after 100 ms.
 */
int ist8310_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		     size_t len);
int ist8310_reg_write(struct compass_dev *compass, uint8_t reg,
		      uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* IST8310_H */
