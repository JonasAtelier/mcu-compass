/*
 * LIS3MDL register map. Every value cites ST's datasheet, DocID024204 Rev 4,
 * which is in docs/LIS3MDL.pdf (cited as DS DocID024204 Rev 4).
 *
 * The datasheet gives no time for SOFT_RST or REBOOT to finish, so the
 * driver never uses them: probe writes every CTRL register outright instead,
 * which leaves the part in a known state without waiting on a figure that is
 * not there.
 */
#ifndef LIS3MDL_REGS_H
#define LIS3MDL_REGS_H

/*
 * The I2C addresses, §5.1.1 and Table 11, are public: LIS3MDL_ADDR_LOW and
 * LIS3MDL_ADDR_HIGH in include/lis3mdl.h.
 */

/*
 * How the register address goes on the wire.
 * I2C, §5.1.1: SUB(7) set auto-increments for a multi-byte read or write.
 * SPI, §5.2 and Figure 6: bit 0 (sent first, so the byte's MSb) is RW, 1 to
 * read; bit 1 is MS, 1 to auto-increment; bits 2-7 are AD(5:0).
 */
#define LIS3MDL_I2C_AUTO_INCREMENT	0x80
#define LIS3MDL_SPI_READ		0x80
#define LIS3MDL_SPI_AUTO_INCREMENT	0x40
#define LIS3MDL_SPI_ADDRESS_MASK	0x3F

/* §7.1, Table 16 and Table 17: WHO_AM_I, 00111101b. */
#define LIS3MDL_WHO_AM_I		0x0F
#define LIS3MDL_WHO_AM_I_VALUE		0x3D

/*
 * §7.2, Tables 18-22: CTRL_REG1 = TEMP_EN OM1 OM0 DO2 DO1 DO0 FAST_ODR ST.
 * OM is the X/Y operating mode (Table 21), DO the output data rate
 * (Table 22). FAST_ODR and ST stay 0.
 */
#define LIS3MDL_CTRL_REG1		0x20
#define LIS3MDL_TEMP_EN			(1u << 7)
#define LIS3MDL_OM_SHIFT		5
#define LIS3MDL_DO_SHIFT		2

/*
 * §7.3, Tables 23-25: CTRL_REG2 = 0 FS1 FS0 0 REBOOT SOFT_RST 0 0, every 0
 * bit required. FS is the full scale (Table 25).
 */
#define LIS3MDL_CTRL_REG2		0x21
#define LIS3MDL_FS_SHIFT		5

/*
 * §7.4, Tables 26-28: CTRL_REG3 = 0 0 LP 0 0 SIM MD1 MD0. LP 0 lets DO set
 * the rate (Table 27), SIM 0 is 4-wire SPI, MD 00 continuous conversion and
 * 11 power-down (Table 28).
 */
#define LIS3MDL_CTRL_REG3		0x22
#define LIS3MDL_MD_CONTINUOUS		0x00
#define LIS3MDL_MD_POWER_DOWN		0x03

/*
 * §7.5, Tables 29-31: CTRL_REG4 = 0 0 0 0 OMZ1 OMZ0 BLE 0. OMZ is the Z
 * operating mode (Table 31); BLE 0 keeps the LSb at the lower address.
 */
#define LIS3MDL_CTRL_REG4		0x23
#define LIS3MDL_OMZ_SHIFT		2

/*
 * §7.6, Tables 32-33: CTRL_REG5 = FAST_READ BDU 0 0 0 0 0 0. BDU holds the
 * output registers until both bytes of a value are read, so a burst never
 * mixes the halves of two samples.
 */
#define LIS3MDL_CTRL_REG5		0x24
#define LIS3MDL_BDU			(1u << 6)

/* §7.7, Tables 34-35: STATUS_REG. ZYXDA, a new X, Y and Z set. */
#define LIS3MDL_STATUS_REG		0x27
#define LIS3MDL_ZYXDA			(1u << 3)

/*
 * §7.8-§7.10 and Table 16: OUT_X_L 28h to OUT_Z_H 2Dh, two's complement,
 * low byte first with BLE 0. One six-byte auto-increment burst.
 */
#define LIS3MDL_OUT_X_L			0x28
#define LIS3MDL_DATA_LEN		6

/* §7.11 and Table 16: TEMP_OUT_L 2Eh, TEMP_OUT_H 2Fh, two's complement. */
#define LIS3MDL_TEMP_OUT_L		0x2E

/* Table 25 and Table 21/31: the highest code of each two-bit field. */
#define LIS3MDL_FIELD_CODE_MAX		3
/* Table 22: DO codes 000-111. */
#define LIS3MDL_DO_CODE_MAX		7

#endif /* LIS3MDL_REGS_H */
