/*
 * GY-271: find out which compass chip this board actually carries, then read
 * it. Boards sold as "GY-271 HMC5883L" have shipped three different parts:
 *
 *   0x2C  QMC5883P  -- most boards made now
 *   0x0D  QMC5883L  -- most boards of the last few years
 *   0x1E  HMC5883L  -- Honeywell's original, discontinued; not supported
 *
 * Each address is fixed by the chip, so the one that ACKs names it. A probe
 * that finds nothing returns -ENXIO and says nothing unless COMPASS_DEBUG is
 * on; any other error means something answered and was refused, or the bus
 * itself failed, and probe logged why.
 *
 * A snippet, not a project: the clock tree, the printf() retarget and the bus
 * are the app's. Pins are examples -- I2C1 on PB8 (SCL) and PB9 (SDA), AF4 --
 * any pair your chip routes to an I2C peripheral works. Written for the F4,
 * whose I2C takes Init.ClockSpeed; an F7 sets Init.Timing instead, as CubeMX
 * computes it. Needs HAL_I2C_MODULE_ENABLED, COMPASS_QMC5883L = y and
 * COMPASS_QMC5883P = y; printing the floats needs -u _printf_float.
 * compass_heading_deg() needs libm: stm-forge links it, other builds add -lm.
 */
#include <errno.h>
#include <stdio.h>

#include CMSIS_device_header

#include "compass.h"
#include "compass_log.h"
#include "qmc5883l.h"
#include "qmc5883p.h"

#define HMC5883L_ADDR	0x1E

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
	i2c1.Init.ClockSpeed = 400000;
	i2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	i2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	HAL_I2C_Init(&i2c1);
}

/* Newest part first: a board made today most likely carries the P. */
static int probe_any(struct compass_dev *compass, I2C_HandleTypeDef *bus)
{
	int ret;

	ret = qmc5883p_probe(compass, bus, NULL);
	if (ret != -ENXIO)
		return ret;
	ret = qmc5883l_probe(compass, bus, NULL);
	if (ret != -ENXIO)
		return ret;

	if (HAL_I2C_IsDeviceReady(bus, HMC5883L_ADDR << 1, 1, 100) == HAL_OK)
		printf("gy271: a device answers at 0x%02x: that is an HMC5883L, "
		       "which this library does not drive\n", HMC5883L_ADDR);
	else
		printf("gy271: nothing at 0x%02x, 0x%02x or 0x%02x -- check "
		       "power, SDA/SCL and pull-ups\n", QMC5883P_ADDR,
		       QMC5883L_ADDR, HMC5883L_ADDR);
	return -ENXIO;
}

int main(void)
{
	static struct compass_dev compass;	/* zeroed */

	HAL_Init();
	/* SystemClock_Config() and the UART behind printf() go here. */
	i2c1_init();

	if (probe_any(&compass, &i2c1))
		for (;;)
			;
	printf("gy271: this GY-271 carries a %s\n", compass.name);

	/*
	 * From here on nothing depends on which chip it was. The defaults
	 * differ, though: the L starts at +/-200 uT, the P at +/-3000 uT.
	 */
	for (;;) {
		struct compass_sample s;

		if (!compass_read(&compass, &s)) {
			compass_log_sample(&compass, &s);
			compass_info(&compass, "heading %5.1f deg (uncalibrated)",
				     (double)compass_heading_deg(s.field, 0.0f));
		}
		HAL_Delay(500);
	}
}
