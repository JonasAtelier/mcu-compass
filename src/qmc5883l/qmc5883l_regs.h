/*
 * QMC5883L register addresses and bit fields.
 *
 * Every address, bit position and constant below appears in two independent
 * drivers that agree: the Linux kernel's drivers/iio/magnetometer/qmc5883l.c
 * and ESPHome's components/qmc5883l/. No datasheet ships in docs/ for this
 * part.
 *
 * This is NOT an HMC5883L. Different manufacturer, different I2C address
 * (0x0D against 0x1E), different register map. Nor is it the QMC5883P, which
 * sits at 0x2C with yet another map -- see the README.
 */
#ifndef QMC5883L_REGS_H
#define QMC5883L_REGS_H

/* Data, X then Y then Z, each LOW byte first. Little endian, unlike most. */
#define QMC5883L_DATA_X_LSB	0x00
#define QMC5883L_DATA_LEN	6

#define QMC5883L_STATUS		0x06
#define QMC5883L_TEMP_LSB	0x07	/* signed, little endian, 100 LSB per degree */
#define QMC5883L_CTRL1		0x09
#define QMC5883L_CTRL2		0x0A
#define QMC5883L_SET_RESET	0x0B
#define QMC5883L_CHIP_ID	0x0D	/* same number as the address, by coincidence */

/*
 * The identity register reads 0xFF -- which is also what a floating SDA line
 * reads when nothing is on the bus at all. Checking it proves nothing on its
 * own, so the driver additionally writes and reads back SET_RESET.
 */
#define QMC5883L_CHIP_ID_VALUE	0xFF

/* ---- STATUS (0x06) ---- */
#define QMC5883L_STATUS_DRDY	(1u << 0)
#define QMC5883L_STATUS_OVL	(1u << 1)	/* a field beyond the selected range */

/* ---- CTRL1 (0x09) ---- */
#define QMC5883L_MODE_SHIFT	0	/* bits 1:0 */
#define QMC5883L_ODR_SHIFT	2	/* bits 3:2 */
#define QMC5883L_RNG_SHIFT	4	/* bits 5:4 */
#define QMC5883L_OSR_SHIFT	6	/* bits 7:6 */

#define QMC5883L_MODE_STANDBY	0x00
#define QMC5883L_MODE_CONT	0x01

/* ---- CTRL2 (0x0A) ---- */
#define QMC5883L_INT_ENB	(1u << 0)	/* active low: set to DISABLE the pin */
#define QMC5883L_ROL_PNT	(1u << 6)
#define QMC5883L_SOFT_RST	(1u << 7)

/*
 * SET/RESET period. Both reference drivers write 0x01 and call it the
 * recommended value; the datasheet gives no other. The part does not measure
 * reliably without it.
 */
#define QMC5883L_SET_RESET_VAL	0x01

/*
 * Wait after a soft reset. Carried over from esp-qmc5883l-mag; neither
 * reference driver states a figure, so this is a generous guess, not a spec.
 */
#define QMC5883L_RESET_WAIT_MS	10

#endif /* QMC5883L_REGS_H */
