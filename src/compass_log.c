#include <stdint.h>

#include "compass_log.h"

/*
 * Decimal text of v, written backwards from end. newlib-nano's printf, the
 * one stm-forge links, has no %lld, so the timestamp cannot go through PRId64.
 */
static const char *compass_format_i64(char *end, int64_t v)
{
	uint64_t u = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;

	*--end = '\0';
	do {
		*--end = (char)('0' + u % 10);
		u /= 10;
	} while (u);
	if (v < 0)
		*--end = '-';
	return end;
}

void compass_log_sample(const struct compass_dev *compass,
			const struct compass_sample *s)
{
	char t[21];	/* "-9223372036854775808" and its NUL */

	compass_info(compass, "t %s us  x %+8.2f  y %+8.2f  z %+8.2f uT",
		     compass_format_i64(t + sizeof(t), s->t_us),
		     (double)s->field[0], (double)s->field[1],
		     (double)s->field[2]);
}
