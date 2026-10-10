/*
 * compass -- a small Linux-IIO-style magnetometer subsystem for ESP-IDF and
 * the STM32 HAL.
 *
 * One struct compass_dev per sensor. A chip driver's probe fills in its name,
 * its ops and its private state; compass_read() does the rest: timestamp, the
 * chip's own read, then the mount rotation into the board frame. Code above
 * this header sees one shape of sample whichever chip produced it.
 *
 * The field is in microtesla: SI like the imu library, and the earth's 25-65 uT reads
 * as a plain number. 1 gauss is 100 uT.
 *
 * Every int-returning function returns 0 or a negative errno.
 */
#ifndef COMPASS_H
#define COMPASS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct compass_sample {
	float field[3];		/* X, Y, Z in microtesla */
	int64_t t_us;		/* compass_time_us() at read */
};

struct compass_dev;

struct compass_ops {
	/*
	 * Chip frame, microtesla; must not touch s->t_us. A reading the chip
	 * flags as clipped is -ERANGE, not a sample: it would look normal.
	 */
	int (*read)(struct compass_dev *compass, struct compass_sample *s);
};

/*
 * Must start zeroed -- static storage or `= { 0 }` -- because probe treats a
 * non-NULL ops or priv as "already bound" and returns -EBUSY rather than
 * leak the first binding. remove, or a failed probe, zeroes them again, so
 * the same compass_dev can be probed afresh.
 */
struct compass_dev {
	const char *name;		/* "qmc5883p"; log prefix */
	const struct compass_ops *ops;
	void *priv;			/* chip state, owned by probe/remove */
	int8_t mount[3][3];		/* chip->board; all-zero = identity */
};

/*
 * Timestamp, ops->read, then board[i] = sum_j mount[i][j] * chip[j]. *s is
 * written only on success.
 */
int compass_read(struct compass_dev *compass, struct compass_sample *s);

/*
 * The clock compass_read() stamps samples with, in microseconds. On ESP-IDF
 * it is esp_timer_get_time(). On the STM32 HAL the default is weak and only
 * HAL_GetTick() * 1000: millisecond steps that wrap after 49.7 days. Define your own compass_time_us() for real microseconds -- the
 * DWT->CYCCNT cycle counter divided by the core clock in MHz, or a free
 * running 32-bit timer at 1 MHz extended to 64 bits.
 */
int64_t compass_time_us(void);

/*
 * Heading in degrees, 0 at north and growing clockwise, in [0, 360).
 *
 * field is in the board frame: X to the right, Y forward, Z up -- so the
 * heading is where +Y points. Set compass_dev.mount to get there.
 *
 * Only true with the board held flat: tilt it and X and Y pick up the
 * vertical field, which in temperate latitudes is most of the field. Tilt
 * compensation needs an accelerometer and is the caller's job. So is the
 * hard-iron offset -- subtract it from field[] first; out of the box the
 * steel around the sensor outweighs the earth.
 *
 * declination_deg turns magnetic north into true north. It depends on where
 * you are and drifts yearly; look it up.
 *
 * Needs libm (atan2f, fmodf): ESP-IDF and stm-forge link it, other builds
 * add -lm.
 */
float compass_heading_deg(const float field[3], float declination_deg);

#ifdef __cplusplus
}
#endif

#endif /* COMPASS_H */
