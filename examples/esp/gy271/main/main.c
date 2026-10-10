/*
 * GY-271: find out which compass chip this board actually carries, then read
 * it. Boards sold as "GY-271 HMC5883L" have shipped three different parts:
 *
 *   0x2C  QMC5883P  -- most boards made now
 *   0x0D  QMC5883L  -- most boards of the last few years
 *   0x1E  HMC5883L  -- Honeywell's original, discontinued; not supported
 *
 * Each address is fixed by the chip, so the one that ACKs names it. A probe
 * that finds nothing returns -ENXIO and says nothing above debug level; any
 * other error means something answered and was refused, and probe logged why.
 */
#include <errno.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compass.h"
#include "compass_log.h"
#include "qmc5883l.h"
#include "qmc5883p.h"

#define PIN_SDA 21
#define PIN_SCL 22

#define HMC5883L_ADDR	0x1E

static const char *TAG = "gy271";

/* Newest part first: a board made today most likely carries the P. */
static int probe_any(struct compass_dev *compass, i2c_master_bus_handle_t bus)
{
	int ret;

	ret = qmc5883p_probe(compass, bus, NULL);
	if (ret != -ENXIO)
		return ret;
	ret = qmc5883l_probe(compass, bus, NULL);
	if (ret != -ENXIO)
		return ret;

	if (i2c_master_probe(bus, HMC5883L_ADDR, 100) == ESP_OK)
		ESP_LOGE(TAG, "a device answers at 0x%02x: that is an HMC5883L, "
			 "which this library does not drive", HMC5883L_ADDR);
	else
		ESP_LOGE(TAG, "nothing at 0x%02x, 0x%02x or 0x%02x -- check "
			 "power, SDA/SCL and pull-ups", QMC5883P_ADDR,
			 QMC5883L_ADDR, HMC5883L_ADDR);
	return -ENXIO;
}

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
	struct compass_dev compass = { 0 };
	i2c_master_bus_handle_t bus;

	ESP_ERROR_CHECK(i2c_new_master_bus(&buscfg, &bus));

	if (probe_any(&compass, bus))
		return;
	ESP_LOGI(TAG, "this GY-271 carries a %s", compass.name);

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
		vTaskDelay(pdMS_TO_TICKS(500));
	}
}
