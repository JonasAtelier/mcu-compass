/*
 * The CMPS14 conversions and command bytes against docs/cmps14.pdf, typed in
 * again here from its pages rather than copied from the header.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "cmps14.h"
#include "cmps14_cmds.h"
#include "cmps14_math.h"

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-4f;
}

int main(void)
{
	static const uint8_t be[2] = { 0x12, 0x34 };
	static const uint8_t neg[2] = { 0xFF, 0xF0 };

	/* p.6: high byte first, signed. */
	assert(cmps14_sample(be) == 0x1234);
	assert(cmps14_sample(neg) == -16);

	/* p.3, p.6: uTesla, Q point 4. */
	assert(near(cmps14_ut(16), 1.0f));
	assert(near(cmps14_ut(-16), -1.0f));
	assert(near(cmps14_ut(1), 0.0625f));

	/* p.2: 0-3599 tenths of a degree. */
	assert(near(cmps14_bearing(3599), 359.9f));
	assert(near(cmps14_bearing(1), 0.1f));

	/* p.6 command table. */
	assert(CMPS14_GET_VERSION == 0x11 && CMPS14_GET_BEARING_16 == 0x13 &&
	       CMPS14_GET_MAGNETOMETER == 0x19 &&
	       CMPS14_GET_CALIBRATION_STATE == 0x24);
	assert(CMPS14_CAL_CONFIG_1 == 0x98 && CMPS14_CAL_CONFIG_2 == 0x95 &&
	       CMPS14_CAL_CONFIG_3 == 0x99);
	assert(CMPS14_STORE_1 == 0xF0 && CMPS14_STORE_2 == 0xF5 &&
	       CMPS14_STORE_3 == 0xF6);
	assert(CMPS14_ERASE_1 == 0xE0 && CMPS14_ERASE_2 == 0xE5 &&
	       CMPS14_ERASE_3 == 0xE2);
	assert(CMPS14_OK == 0x55 && CMPS14_MAGNETOMETER_LEN == 6);

	/* p.7 setup byte: bit 7 set, bits 6, 5, 3 undefined, the rest ours. */
	assert(CMPS14_SETUP_ALWAYS == 0x80 && CMPS14_SETUP_UNDEFINED == 0x68);
	assert((CMPS14_CAL_MAG | CMPS14_CAL_ACCEL | CMPS14_CAL_GYRO |
		CMPS14_CAL_AUTOSAVE) == 0x17);
	assert(((CMPS14_CAL_MAG | CMPS14_CAL_ACCEL | CMPS14_CAL_GYRO |
		 CMPS14_CAL_AUTOSAVE) &
		(CMPS14_SETUP_ALWAYS | CMPS14_SETUP_UNDEFINED)) == 0);
	assert(CMPS14_ERASE_WAIT_MS == 300);

	printf("all cmps14 math checks passed\n");
	return 0;
}
