/*
 * QMC5883L: field in microtesla, and a heading once you have calibrated,
 * through the compass core.
 *
 * The heading printed here is only as good as HARD_IRON_* below. Straight out
 * of the box those are zero and the heading will be wrong -- see the README
 * for the two-minute calibration that fixes it.
 *
 * Not sure your GY-271 has an L on it? Run the gy271 example first.
 */
#include <errno.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_log.h"
#include "qmc5883l.h"

#define PIN_SDA 21
#define PIN_SCL 22

/* Fill these in from the calibration spin. Units are microtesla. */
#define HARD_IRON_X	0.0f
#define HARD_IRON_Y	0.0f

/* Magnetic declination where you are, in degrees. Look it up; it drifts. */
#define DECLINATION	0.0f

void app_main(void)
{
	i2c_master_bus_config_t buscfg = {
		.i2c_port = -1,
		.sda_io_num = PIN_SDA,
		.scl_io_num = PIN_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	struct qmc5883l_config cfg = {
		.range = QMC5883L_RANGE_200UT,	/* the earth's field is under 70 uT */
		.odr = QMC5883L_ODR_100HZ,
		.osr = QMC5883L_OSR_512,
	};
	struct compass_dev compass = { 0 };
	i2c_master_bus_handle_t bus;

	ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &bus));

	if (qmc5883l_probe(&compass, bus, &cfg))
		return;		/* probe already logged why, under "compass" */

	if (HARD_IRON_X == 0.0f && HARD_IRON_Y == 0.0f)
		compass_warn(&compass, "hard-iron offsets are zero; the heading "
			     "will be wrong");

	for (;;) {
		struct compass_sample s;
		int ret = compass_read(&compass, &s);

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
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}
