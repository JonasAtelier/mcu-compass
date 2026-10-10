/*
 * cmps14 -- Robot Electronics CMPS14 tilt-compensated compass module, as a
 * compass chip driver, in its serial mode.
 *
 * A BNO080 behind a small processor: it calibrates itself for hard and soft
 * iron and answers in uTesla, and it also does the tilt-compensated bearing
 * this library otherwise leaves to you.
 *
 * Serial only. The module also has an I2C mode, picked by its mode pin at
 * power-up (p.1); this driver does not support it. For serial, tie the mode
 * pin to 0 V before power-up. The link is 9600 bps, 8 data bits, no parity,
 * 1 stop bit, at 3.3-5 V logic levels (p.5) -- never RS232.
 *
 * Samples come out of compass_read(). Everything below that is CMPS14
 * specific takes the struct compass_dev that cmps14_probe() filled, and
 * returns -EINVAL if handed a device some other driver probed. Its outputs
 * (*state, *degrees, field[], ...) are undefined when it returns an error.
 *
 * The serial protocol has no checksum. Stale input is dropped before each
 * command, but that is best effort: an answer that arrives after its
 * command timed out can still land in the next one and shift it.
 *
 * "CMPS14 - documentation" is in docs/cmps14.pdf; every command byte in
 * src/cmps14_cmds.h and cmps14.c cites its page. Nothing here has been run
 * against a physical module.
 */
#ifndef CMPS14_H
#define CMPS14_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "driver/uart.h"
#else
#include CMSIS_device_header
#endif

#include "compass.h"

#ifdef __cplusplus
extern "C" {
#endif

/* p.7: the setup byte's background-calibration switches. */
#define CMPS14_CAL_MAG		(1u << 0)
#define CMPS14_CAL_ACCEL	(1u << 1)
#define CMPS14_CAL_GYRO		(1u << 2)	/* p.7: off for very still use */
#define CMPS14_CAL_AUTOSAVE	(1u << 4)	/* periodic save of the profile */

/*
 * uart is, on ESP-IDF, a port whose driver the app has already installed
 * and configured; on the STM32 HAL a handle already HAL_UART_Init()ed. Either
 * way at 9600 bps, 8N1 (p.5); the line is the app's, not this driver's.
 *
 * Ask for the software version (p.6, 0x11) and bind. The module has no
 * identity register, so any one-byte answer is taken as a CMPS14. Fills
 * compass->name, ops and priv; leaves compass->mount alone.
 *
 * -EBUSY if compass is already bound (see struct compass_dev).
 * -ENXIO if nothing answers within 100 ms, decided before anything is
 * allocated.
 * -EINVAL for a NULL compass or handle, or on ESP-IDF a port with no driver
 * installed.
 * Other bus failures come back as the errno of the failed transfer.
 */
int cmps14_probe(struct compass_dev *compass,
#ifdef ESP_PLATFORM
		 uart_port_t uart
#else
		 UART_HandleTypeDef *uart
#endif
		 );

/* Releases it. The module has nothing to power down. */
void cmps14_remove(struct compass_dev *compass);

/* The software version probe read. */
int cmps14_version(struct compass_dev *compass, uint8_t *version);

/* The magnetometer answer as counts, 16 per uT (p.6), chip frame. */
int cmps14_read_raw(struct compass_dev *compass, int16_t field[3]);

/*
 * The module's own tilt-compensated bearing, 0 to 359.9 degrees (p.2, p.6
 * 0x13). Unlike compass_heading_deg() it needs no flat board, but it is in
 * the module's frame and knows nothing of compass_dev.mount.
 */
int cmps14_bearing_deg(struct compass_dev *compass, float *degrees);

/*
 * p.6 0x24: two bits each, 0 uncalibrated to 3 fully, from bit 0 up:
 * magnetometer, accelerometer, gyro, system (p.7). p.7 notes the gyro field
 * does not work, a bug in the sensor.
 */
int cmps14_calibration_state(struct compass_dev *compass, uint8_t *state);

/*
 * Background calibration on or off: an OR of CMPS14_CAL_*, 0 for all off
 * (p.7). Sends 0x98, 0x95, 0x99, each answered 0x55, then the setup byte,
 * and waits for a 0x55 to that too -- p.7 gives it none, so silence is also
 * success. -EINVAL for any other bit; -EIO if an answer is not 0x55.
 */
int cmps14_set_calibration(struct compass_dev *compass, uint8_t enable);

/* p.7: keep the current calibration profile across power cycles. */
int cmps14_store_calibration(struct compass_dev *compass);

/*
 * p.7: erase the stored profile. The last byte resets the BNO080, so this
 * then waits the 300 ms p.7 recommends.
 */
int cmps14_erase_calibration(struct compass_dev *compass);

/*
 * Escape hatch for the commands this driver does not model -- pitch, roll,
 * acceleration, gyro (p.6): send cmd, read len answer bytes. Changing the
 * baud rate (0xA0, 0xA1) is the app's to follow with its own UART, so
 * through here too.
 */
int cmps14_command(struct compass_dev *compass, uint8_t cmd, void *answer,
		   size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CMPS14_H */
