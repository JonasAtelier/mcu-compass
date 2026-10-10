/*
 * IST8310: field in microtesla, and a heading once you have calibrated,
 * through the compass core.
 *
 * The heading printed here is only as good as HARD_IRON_* below. Straight out
 * of the box those are zero and the heading will be wrong -- see the README
 * for the two-minute calibration that fixes it.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the bus
 * are the app's. Pins are examples -- I2C1 on PB8 (SCL) and PB9 (SDA), AF4 --
 * any pair your chip routes to an I2C peripheral works. Written for the F4,
 * whose I2C takes Init.ClockSpeed; an F7 sets Init.Timing instead, as CubeMX
 * computes it. Needs HAL_I2C_MODULE_ENABLED and COMPASS_IST8310 = y;
 * printing the floats needs -u _printf_float. compass_heading_deg() needs
 * libm: stm-forge links it, other builds add -lm.
 *
 * Wiring: I2C1, CAD0 to GND and CAD1 to VDD or open (0x0E), DRDY left
 * open. See the README.
 */
#include <errno.h>

#include CMSIS_device_header

#include "compass.h"
#include "compass_log.h"
#include "ist8310.h"

/* Fill these in from the calibration spin. Units are microtesla. */
#define HARD_IRON_X	0.0f
#define HARD_IRON_Y	0.0f

/* Magnetic declination where you are, in degrees. Look it up; it drifts. */
#define DECLINATION	0.0f

static I2C_HandleTypeDef i2c1;

/* The app's half: pins, peripheral clock and bus speed. */
static void i2c1_init(void)
{
	GPIO_InitTypeDef pins = {
		.Pin = GPIO_PIN_8 | GPIO_PIN_9,
		.Mode = GPIO_MODE_AF_OD,
		.Pull = GPIO_PULLUP,	/* weak; a module carries its own */
		.Speed = GPIO_SPEED_FREQ_HIGH,
		.Alternate = GPIO_AF4_I2C1,
	};

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_I2C1_CLK_ENABLE();
	HAL_GPIO_Init(GPIOB, &pins);

	i2c1.Instance = I2C1;
	i2c1.Init.ClockSpeed = 400000;
	i2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	i2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	HAL_I2C_Init(&i2c1);
}

int main(void)
{
	struct ist8310_config cfg = {
		.address = IST8310_ADDR_HL,
		.avg_xz = IST8310_AVG_16,	/* §3.1.1's low-noise setting */
		.avg_y = IST8310_AVG_16,
	};
	static struct compass_dev compass;	/* zeroed */

	HAL_Init();
	/* SystemClock_Config() and the UART behind printf() go here. */
	i2c1_init();

	if (ist8310_probe(&compass, &i2c1, &cfg))
		for (;;)
			;	/* probe already logged why, as "compass: ist8310: ..." */

	if (HARD_IRON_X == 0.0f && HARD_IRON_Y == 0.0f)
		compass_warn(&compass, "hard-iron offsets are zero; the heading "
			     "will be wrong");

	for (;;) {
		struct compass_sample s;
		int ret = compass_read(&compass, &s);

		if (ret == -ERANGE) {
			compass_warn(&compass, "field beyond range -- move the "
				     "magnet away");
		} else if (!ret) {	/* -ENODATA: not measured yet */
			s.field[0] -= HARD_IRON_X;
			s.field[1] -= HARD_IRON_Y;
			compass_log_sample(&compass, &s);
			compass_info(&compass, "heading %5.1f deg",
				     (double)compass_heading_deg(s.field,
								 DECLINATION));
		}
		HAL_Delay(200);
	}
}
