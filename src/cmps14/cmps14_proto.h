/*
 * CMPS14 serial commands. Every value cites Robot Electronics' "CMPS14 -
 * documentation" in docs/cmps14.pdf, by page: the serial command table is
 * on p.6, the serial calibration and baud notes on p.7.
 *
 * Serial is command and answer: one command byte out, a fixed number of
 * bytes back (p.6, "Bytes returned"). There are no registers.
 */
#ifndef CMPS14_PROTO_H
#define CMPS14_PROTO_H

/* p.6: GET VERSION, 1 byte. */
#define CMPS14_GET_VERSION		0x11

/* p.6: GET BEARING 16 BIT, 2 bytes, high first, 0-3599 (p.2: tenths). */
#define CMPS14_GET_BEARING_16		0x13

/*
 * p.6: GET MAGNETOMETER, 6 bytes, X high, X low, Y high, Y low, Z high, Z
 * low; signed, uTesla, Q point 4. p.3: already calibrated for hard and soft
 * iron.
 */
#define CMPS14_GET_MAGNETOMETER		0x19
#define CMPS14_MAGNETOMETER_LEN		6

/* p.6: GET CALIBRATION STATE, 1 byte; the 2-bit fields are on p.7. */
#define CMPS14_GET_CALIBRATION_STATE	0x24

/* p.6 and p.7: every byte of the three sequences below answers 0x55. */
#define CMPS14_OK			0x55

/*
 * p.6 and p.7: CHANGE CALIBRATION CONFIG BYTE 1-3, then the setup byte.
 * p.7 gives the setup byte's bits but no answer for it.
 */
#define CMPS14_CAL_CONFIG_1		0x98
#define CMPS14_CAL_CONFIG_2		0x95
#define CMPS14_CAL_CONFIG_3		0x99

/* p.7: the setup byte. Bit 7 always 1; bits 6, 5 and 3 undefined. */
#define CMPS14_SETUP_ALWAYS		0x80
#define CMPS14_SETUP_UNDEFINED		0x68

/* p.6 and p.7: STORE CALIBRATION BYTE 1-3. */
#define CMPS14_STORE_1			0xF0
#define CMPS14_STORE_2			0xF5
#define CMPS14_STORE_3			0xF6

/*
 * p.6 and p.7: DELETE CALIBRATION BYTE 1-3. The last one resets the BNO080,
 * after which p.7 recommends 300 ms.
 */
#define CMPS14_ERASE_1			0xE0
#define CMPS14_ERASE_2			0xE5
#define CMPS14_ERASE_3			0xE2
#define CMPS14_ERASE_WAIT_MS		300

#endif /* CMPS14_PROTO_H */
