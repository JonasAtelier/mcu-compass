/*
 * QMC5883P register map. Every value cites QST's datasheet, QST-PD-B002-22
 * Rev E, which is in docs/QMC5883P_RevE.pdf.
 *
 * Not the QMC5883L's map shifted by one: the data starts at 0x01 rather than
 * 0x00, status moves to 0x09, range moves from CTRL1 into CTRL2, there are
 * two oversampling fields, and the identity register holds a real ID.
 */
#ifndef QMC5883P_REGS_H
#define QMC5883P_REGS_H

/* Table 14 / §9.2.1: CHIPID, default value 80H. */
#define QMC5883P_CHIP_ID		0x00
#define QMC5883P_CHIP_ID_VALUE		0x80

/*
 * Table 15 / §9.2.1: X, Y, Z, each LSB then MSB, 16-bit two's complement,
 * saturating at -32768 and 32767. One burst: §8.2.4's read keeps sending
 * bytes for as long as the master ACKs them.
 */
#define QMC5883P_DATA_X_LSB		0x01
#define QMC5883P_DATA_LEN		6

/*
 * Table 16 / §9.2.2: both flags clear when the status register is read.
 * OVFL means some axis passed +/-30000 counts -- which is exactly full scale
 * on every range (Table 2 sensitivity x Table 18 range).
 */
#define QMC5883P_STATUS			0x09
#define QMC5883P_STATUS_DRDY		(1u << 0)
#define QMC5883P_STATUS_OVFL		(1u << 1)
#define QMC5883P_OVFL_COUNTS		30000

/* Table 17: CTRL1 = OSR2<7:6> OSR1<5:4> ODR<3:2> MODE<1:0>. */
#define QMC5883P_CTRL1			0x0A
#define QMC5883P_MODE_SHIFT		0
#define QMC5883P_ODR_SHIFT		2
#define QMC5883P_OSR1_SHIFT		4
#define QMC5883P_OSR2_SHIFT		6

/*
 * Table 17 MODE codes. §9.2.3: suspend has to sit between any two of
 * normal, single and continuous. §6.2.4: suspend is also where POR and soft
 * reset leave the part.
 */
#define QMC5883P_MODE_SUSPEND		0x0
#define QMC5883P_MODE_NORMAL		0x1
#define QMC5883P_MODE_SINGLE		0x2	/* not used by this driver */
#define QMC5883P_MODE_CONTINUOUS	0x3

/* §7.4: suspend is the whole of CTRL1 written to zero. */
#define QMC5883P_SUSPEND_CTRL1		0x00

/* Table 18: CTRL2 = SOFT_RST<7> SELF_TEST<6> RFU<5:4> RNG<3:2> SR<1:0>. */
#define QMC5883P_CTRL2			0x0B
#define QMC5883P_SR_SHIFT		0
#define QMC5883P_RNG_SHIFT		2
#define QMC5883P_SELF_TEST		(1u << 6)	/* auto clears, Table 18 */
#define QMC5883P_SOFT_RST		(1u << 7)	/* §7.6 writes 0x80 */

/*
 * Table 18 SET/RESET code 00: set and reset both on. §9.2.3: in the other
 * two modes the offset is not renewed while measuring, so this driver never
 * leaves 00.
 */
#define QMC5883P_SR_ON			0x0

/*
 * §7.1, §7.2 and §7.3 all start by writing 06H to register 29H, "define the
 * sign for X Y and Z axis". Register 29H is not in the register map (Table
 * 14) and nothing else documents it; this driver writes it because every
 * setup sequence QST gives does, and the axis directions in Figure 2 are
 * presumably the ones it selects.
 */
#define QMC5883P_AXIS_SIGN		0x29
#define QMC5883P_AXIS_SIGN_VALUE	0x06

/*
 * Table 7: PORT, power on to ready for I2C, 250 us max. §5 gives no time for
 * a soft reset (§7.6), so the driver waits the power-on figure.
 */
#define QMC5883P_RESET_WAIT_US		250

/* §7.3 self-test: continuous mode, stimulus on, then wait 5 ms (step 6). */
#define QMC5883P_SELF_TEST_CTRL1	0x03	/* step 2 */
#define QMC5883P_SELF_TEST_CTRL2	0x40	/* step 5: also RNG 30 G, SR 00 */
#define QMC5883P_SELF_TEST_WAIT_MS	5

#endif /* QMC5883P_REGS_H */
