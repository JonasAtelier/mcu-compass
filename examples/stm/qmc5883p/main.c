/*
 * QMC5883P: the datasheet's self-test once at start-up, then the field in
 * microtesla and a heading, through the compass core.
 *
 * The heading is only as good as HARD_IRON_* below; see the README for the
 * two-minute calibration. Not sure which chip your GY-271 carries? Run
 * the gy271 example first.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the bus
 * are the app's. Pins are examples -- I2C1 on PB8 (SCL) and PB9 (SDA), AF4 --
 * any pair your chip routes to an I2C peripheral works. Written for the F4,
 * whose I2C takes Init.ClockSpeed; an F7 sets Init.Timing instead, as CubeMX
 * computes it. Needs HAL_I2C_MODULE_ENABLED and COMPASS_QMC5883P = y;
 * printing the floats needs -u _printf_float. compass_heading_deg() needs
 * libm: stm-forge links it, other builds add -lm.
 *
 * Wiring: I2C1; the QMC5883P has no DRDY pin. See the README.
 */
#include <errno.h>
#include <stdbool.h>

#include CMSIS_device_header

#include "compass.h"
#include "compass_log.h"
#include "qmc5883p.h"

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
		.Pull = GPIO_PULLUP,	/* weak; a GY-271 carries its own */
		.Speed = GPIO_SPEED_FREQ_HIGH,
		.Alternate = GPIO_AF4_I2C1,
	};

	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_I2C1_CLK_ENABLE();
	HAL_GPIO_Init(GPIOB, &pins);

	i2c1.Instance = I2C1;
	i2c1.Init.ClockSpeed = 400000;	/* §5.4: 400 kHz at most */
	i2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	i2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	HAL_I2C_Init(&i2c1);
}

int main(void)
{
	/* §7.1's normal-mode setup: +/-8 G, 200 Hz, the deepest filtering. */
	struct qmc5883p_config cfg = {
		.range = QMC5883P_RANGE_800UT,
		.odr = QMC5883P_ODR_200HZ,
		.osr1 = QMC5883P_OSR1_8,
		.osr2 = QMC5883P_OSR2_8,
	};
	static struct compass_dev compass;	/* zeroed */
	int16_t delta[3];

	HAL_Init();
	/* SystemClock_Config() and the UART behind printf() go here. */
	i2c1_init();

	if (qmc5883p_probe(&compass, &i2c1, &cfg))
		for (;;)
			;	/* probe already logged why, as "compass: qmc5883p: ..." */

	/* No pass/fail threshold exists; a live axis moves well off zero. */
	if (!qmc5883p_self_test(&compass, delta))
		compass_info(&compass, "self-test delta x %d  y %d  z %d counts "
			     "at 1000 LSB/G", delta[0], delta[1], delta[2]);

	if (HARD_IRON_X == 0.0f && HARD_IRON_Y == 0.0f)
		compass_warn(&compass, "hard-iron offsets are zero; the heading "
			     "will be wrong");

	for (;;) {
		struct compass_sample s;
		bool ready = false;
		int ret;

		/* Also skips the stimulated reading the self-test left behind. */
		if (qmc5883p_data_ready(&compass, &ready) || !ready) {
			HAL_Delay(10);
			continue;
		}

		ret = compass_read(&compass, &s);
		if (ret == -ERANGE) {
			compass_warn(&compass, "field beyond range -- widen it "
				     "or move the magnet away");
		} else if (!ret) {
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
