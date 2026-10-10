/*
 * IST8310 register map. Every value cites iSentek's datasheet, Version 1.2,
 * which is in docs/IST8310.pdf.
 *
 * The part has no continuous mode: each measurement is asked for by writing
 * CNTL1, and the part drops back to stand-by once the result is in (§3.1.2).
 */
#ifndef IST8310_REGS_H
#define IST8310_REGS_H

/* §6.4.2: WAI, Device ID, default 10h. */
#define IST8310_WAI			0x00
#define IST8310_WAI_VALUE		0x10

/*
 * §6.4.3: STAT1. DRDY is only set while CNTL2's DREN is, which it is out of
 * reset (§6.4.7). Reading any data register clears both bits (§3.4).
 */
#define IST8310_STAT1			0x02
#define IST8310_STAT1_DRDY		(1u << 0)
#define IST8310_STAT1_DOR		(1u << 1)

/*
 * §6.4.4: X, Y, Z, each low byte then high byte, two's complement. One
 * burst: §6.2's multiple-byte read.
 */
#define IST8310_DATA_X_LSB		0x03
#define IST8310_DATA_LEN		6

/* §6.4.6: CNTL1 MODE<3:0>. 0001 turns back to 0000 by itself (§3.1.2). */
#define IST8310_CNTL1			0x0A
#define IST8310_MODE_STANDBY		0x00
#define IST8310_MODE_SINGLE		0x01

/*
 * §6.4.7: CNTL2. SRST runs the power-on reset routine and clears itself;
 * DREN and DRP are 1 out of reset, which is what this driver relies on.
 */
#define IST8310_CNTL2			0x0B
#define IST8310_CNTL2_SRST		(1u << 0)
#define IST8310_CNTL2_DRP		(1u << 2)
#define IST8310_CNTL2_DREN		(1u << 3)

/*
 * §6.4.10: AVGCNTL = Y<5:3> XZ<2:0>, codes 0-4 for 1, 2, 4, 8, 16 samples;
 * "others: no average". §3.1.1 recommends 24h, 16 on both, for low noise.
 */
#define IST8310_AVGCNTL			0x41
#define IST8310_AVG_XZ_SHIFT		0
#define IST8310_AVG_Y_SHIFT		3
#define IST8310_AVG_CODE_MAX		4

/* §6.4.11 and §3.1.1: PDCNTL<7:6> = 11b, "Normal (please use this setting)". */
#define IST8310_PDCNTL			0x42
#define IST8310_PDCNTL_NORMAL		0xC0

/*
 * §4.5: POR, 50 ms max. §6.4.7 says SRST runs that same routine and gives no
 * time of its own, so the driver waits the power-on figure.
 */
#define IST8310_RESET_WAIT_MS		50

#endif /* IST8310_REGS_H */
