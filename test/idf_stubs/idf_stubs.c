/* The stub definitions every test binary links. */
#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

char stub_log_last[256];
int64_t stub_time_us;
unsigned int stub_tick_hz = 100;	/* ESP-IDF's default CONFIG_FREERTOS_HZ */

int64_t esp_timer_get_time(void)
{
	return stub_time_us;
}

void stub_log(char level, const char *tag, const char *fmt, ...)
{
	va_list ap;
	int n;

	n = snprintf(stub_log_last, sizeof(stub_log_last), "%c %s: ", level,
		     tag);
	va_start(ap, fmt);
	vsnprintf(stub_log_last + n, sizeof(stub_log_last) - n, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", stub_log_last);
}
