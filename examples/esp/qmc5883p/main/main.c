/*
 * QMC5883P: the datasheet's self-test once at start-up, then the field in
 * microtesla and a heading, through the compass core.
 *
 * The heading is only as good as HARD_IRON_* below; see the README for the
 * two-minute calibration. Not sure which chip your GY-271 carries? Run
 * the gy271 example first.
 */
#include <errno.h>
#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_log.h"
#include "qmc5883p.h"

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
	/* §7.1's normal-mode setup: +/-8 G, 200 Hz, the deepest filtering. */
	struct qmc5883p_config cfg = {
		.range = QMC5883P_RANGE_800UT,
		.odr = QMC5883P_ODR_200HZ,
		.osr1 = QMC5883P_OSR1_8,
		.osr2 = QMC5883P_OSR2_8,
	};
	struct compass_dev compass = { 0 };
	i2c_master_bus_handle_t bus;
	int16_t delta[3];

	ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &bus));

	if (qmc5883p_probe(&compass, bus, &cfg))
		return;		/* probe already logged why, under "compass" */

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
			vTaskDelay(pdMS_TO_TICKS(10));
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
		vTaskDelay(pdMS_TO_TICKS(200));
	}
}
