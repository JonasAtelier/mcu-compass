/*
 * Kernel dev_err()-style logging for magnetometer drivers and their users.
 *
 * Every line is prefixed with the device's name, so a line still says which
 * chip it came from.
 *
 * ESP-IDF: everything goes out under the one log tag "compass", so one
 * esp_log_level_set("compass", ...) controls every chip. compass_dbg() is
 * ESP_LOGD, subject to CONFIG_LOG_MAXIMUM_LEVEL; CONFIG_COMPASS_DEBUG is not
 * used.
 *
 * STM32 HAL: every line goes out through printf() as "compass: <name>: ...".
 * Where printf() ends up is the app's business: with newlib, retarget
 * _write() to a UART. There is deliberately no level machinery; compass_dbg()
 * alone is compiled out unless CONFIG_COMPASS_DEBUG is 1 (stm-forge:
 * COMPASS_DEBUG = y).
 */
#ifndef COMPASS_LOG_H
#define COMPASS_LOG_H

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#include <stdio.h>
#endif

#include "compass.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#define COMPASS_LOG_TAG	"compass"

#define compass_err(compass, fmt, ...) \
	ESP_LOGE(COMPASS_LOG_TAG, "%s: " fmt, (compass)->name, ##__VA_ARGS__)
#define compass_warn(compass, fmt, ...) \
	ESP_LOGW(COMPASS_LOG_TAG, "%s: " fmt, (compass)->name, ##__VA_ARGS__)
#define compass_info(compass, fmt, ...) \
	ESP_LOGI(COMPASS_LOG_TAG, "%s: " fmt, (compass)->name, ##__VA_ARGS__)
#define compass_dbg(compass, fmt, ...) \
	ESP_LOGD(COMPASS_LOG_TAG, "%s: " fmt, (compass)->name, ##__VA_ARGS__)

#else /* STM32 HAL */
#define compass_err(compass, fmt, ...) \
	printf("compass: %s: " fmt "\n", (compass)->name, ##__VA_ARGS__)
#define compass_warn(compass, fmt, ...) \
	printf("compass: %s: " fmt "\n", (compass)->name, ##__VA_ARGS__)
#define compass_info(compass, fmt, ...) \
	printf("compass: %s: " fmt "\n", (compass)->name, ##__VA_ARGS__)

#if CONFIG_COMPASS_DEBUG
#define compass_dbg(compass, fmt, ...) \
	printf("compass: %s: " fmt "\n", (compass)->name, ##__VA_ARGS__)
#else
/* Still type-checked, so a debug line cannot rot while it is off. */
#define compass_dbg(compass, fmt, ...) do { \
	if (0) \
		printf("compass: %s: " fmt "\n", (compass)->name, \
		       ##__VA_ARGS__); \
} while (0)
#endif
#endif /* ESP_PLATFORM */

/* One info line: timestamp and the three field components. */
void compass_log_sample(const struct compass_dev *compass,
			const struct compass_sample *s);

#ifdef __cplusplus
}
#endif

#endif /* COMPASS_LOG_H */
