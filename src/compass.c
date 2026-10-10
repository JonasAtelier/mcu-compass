#include <errno.h>
#include <math.h>
#include <string.h>

#include "compass.h"

/* M_PI is not in strict C11, and the tests build with -pedantic. */
#define COMPASS_RAD_TO_DEG	(180.0f / 3.14159265358979323846f)

/* v = mount * v, via a copy so a row never reads already-rotated axes. */
static void compass_rotate(const struct compass_dev *compass, float v[3])
{
	float in[3] = { v[0], v[1], v[2] };
	int i;

	for (i = 0; i < 3; i++)
		v[i] = compass->mount[i][0] * in[0] +
		       compass->mount[i][1] * in[1] +
		       compass->mount[i][2] * in[2];
}

int compass_read(struct compass_dev *compass, struct compass_sample *s)
{
	static const int8_t all_zero[3][3];
	struct compass_sample tmp;
	int64_t t;
	int ret;

	if (!compass || !compass->ops || !compass->ops->read || !s)
		return -EINVAL;

	/* Stamp before the bus transfer: that is closer to the sample. */
	t = compass_time_us();
	ret = compass->ops->read(compass, &tmp);
	if (ret)
		return ret;
	tmp.t_us = t;

	/* A zeroed compass_dev must just work, so all-zero means identity. */
	if (memcmp(compass->mount, all_zero, sizeof(all_zero)))
		compass_rotate(compass, tmp.field);

	*s = tmp;
	return 0;
}

/* atan2(-x, y): +Y alone is north, and -X (north on the left) is east. */
float compass_heading_deg(const float field[3], float declination_deg)
{
	float h;

	if (!field)
		return 0.0f;

	h = atan2f(-field[0], field[1]) * COMPASS_RAD_TO_DEG + declination_deg;
	/* fmodf keeps the sign of the dividend, so shift before wrapping */
	h = fmodf(h, 360.0f);
	if (h < 0.0f)
		h += 360.0f;
	/* -1e-6 + 360 rounds to 360.0f in float; keep the range half-open */
	if (h >= 360.0f)
		h = 0.0f;
	return h;
}
