/*
 * Host-side check of the core: compass_read(), compass_heading_deg() and the
 * log macros, against a fake chip whose ops->read hands back whatever the
 * test put in it.
 *
 * The mount matrix is what is worth guarding. A transposed matrix is a
 * rotation the other way, and with a 90 degree mount that still produces a
 * plausible heading -- just 180 degrees out.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "compass.h"
#include "compass_log.h"

#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-4)

static struct compass_sample fake_sample;
static int fake_ret;
static int fake_calls;

static int fake_read(struct compass_dev *compass, struct compass_sample *s)
{
	(void)compass;
	fake_calls++;
	/* The transfer takes time: a stamp taken after it would show this. */
	stub_tick += 1;
	if (fake_ret)
		return fake_ret;
	memcpy(s->field, fake_sample.field, sizeof(s->field));
	return 0;
}

static const struct compass_ops fake_ops = { .read = fake_read };

static struct compass_dev fake_dev(void)
{
	struct compass_dev compass = { .name = "fake", .ops = &fake_ops };

	fake_sample = (struct compass_sample){ .field = { 1.0f, 2.0f, 3.0f } };
	fake_ret = 0;
	fake_calls = 0;
	return compass;
}

static void test_read_stamps_and_identity(void)
{
	struct compass_dev compass = fake_dev();
	struct compass_sample s = { .t_us = -1 };

	/* The weak default clock: HAL_GetTick() milliseconds, as microseconds. */
	stub_tick = 123456789;
	assert(compass_read(&compass, &s) == 0);
	assert(fake_calls == 1);
	/* Stamped before the read, not after: the fake moved the clock. */
	assert(s.t_us == 123456789000LL);
	assert(stub_tick == 123456789 + 1);
	/* All-zero mount is identity, not a zero vector. */
	assert(s.field[0] == 1.0f && s.field[1] == 2.0f && s.field[2] == 3.0f);

	/* 32-bit milliseconds do not wrap in the 64-bit microseconds. */
	stub_tick = UINT32_MAX;
	assert(compass_read(&compass, &s) == 0);
	assert(s.t_us == (int64_t)UINT32_MAX * 1000);
}

static void test_mount_rotation(void)
{
	/* Chip rotated +90 degrees about Z: chip X is board Y. */
	static const int8_t rot_z[3][3] = {
		{ 0, -1, 0 },
		{ 1,  0, 0 },
		{ 0,  0, 1 },
	};
	/* Chip upside down about Y: X and Z flip. */
	static const int8_t flip[3][3] = {
		{ -1, 0,  0 },
		{  0, 1,  0 },
		{  0, 0, -1 },
	};
	struct compass_dev compass = fake_dev();
	struct compass_sample s;

	memcpy(compass.mount, rot_z, sizeof(rot_z));
	assert(compass_read(&compass, &s) == 0);
	/* board = M * chip: (1,2,3) -> (-2, 1, 3). Transposed would be (2,-1,3). */
	assert(s.field[0] == -2.0f && s.field[1] == 1.0f && s.field[2] == 3.0f);

	memcpy(compass.mount, flip, sizeof(flip));
	assert(compass_read(&compass, &s) == 0);
	assert(s.field[0] == -1.0f && s.field[1] == 2.0f && s.field[2] == -3.0f);

	/*
	 * The rotation must reach the heading: north along chip +X, chip turned
	 * +90 about Z, puts north along board +Y -- heading 0.
	 */
	memcpy(compass.mount, rot_z, sizeof(rot_z));
	fake_sample = (struct compass_sample){ .field = { 30.0f, 0.0f, -40.0f } };
	assert(compass_read(&compass, &s) == 0);
	assert(NEAR(compass_heading_deg(s.field, 0.0f), 0.0f));
}

static void test_errno_passthrough(void)
{
	struct compass_dev compass = fake_dev();
	struct compass_sample s = { .field = { 9.0f, 9.0f, 9.0f }, .t_us = 7 };

	/* Whatever the chip says comes back unchanged, and s is left alone. */
	stub_tick = 42;
	fake_ret = -ERANGE;
	assert(compass_read(&compass, &s) == -ERANGE);
	fake_ret = -ENXIO;
	assert(compass_read(&compass, &s) == -ENXIO);
	fake_ret = -EIO;
	assert(compass_read(&compass, &s) == -EIO);
	fake_ret = -EAGAIN;
	assert(compass_read(&compass, &s) == -EAGAIN);
	fake_ret = -ETIMEDOUT;
	assert(compass_read(&compass, &s) == -ETIMEDOUT);
	assert(s.field[0] == 9.0f && s.field[2] == 9.0f && s.t_us == 7);

	/* Nothing to call is the caller's mistake, not a chip error. */
	fake_calls = 0;
	assert(compass_read(NULL, &s) == -EINVAL);
	assert(compass_read(&compass, NULL) == -EINVAL);
	compass.ops = NULL;
	assert(compass_read(&compass, &s) == -EINVAL);
	assert(fake_calls == 0);
}

/* Moved from esp-qmc5883l-mag's test_math.c with the heading itself. */
static void test_heading(void)
{
	const float north[3] = { 0.0f, 10.0f, -40.0f };
	const float east[3] = { -10.0f, 0.0f, 0.0f };
	const float west[3] = { 10.0f, 0.0f, 0.0f };
	const float south[3] = { 0.0f, -10.0f, 0.0f };

	/* atan2(-x, y): +Y alone is north; Z plays no part. */
	assert(NEAR(compass_heading_deg(north, 0.0f), 0.0f));
	assert(NEAR(compass_heading_deg(east, 0.0f), 90.0f));
	assert(NEAR(compass_heading_deg(south, 0.0f), 180.0f));
	assert(NEAR(compass_heading_deg(west, 0.0f), 270.0f));

	/* Always inside [0, 360), whatever the declination does. */
	for (float dec = -180.0f; dec <= 180.0f; dec += 13.0f) {
		for (float a = 0.0f; a < 6.28f; a += 0.21f) {
			float f[3] = { cosf(a) * 30.0f, sinf(a) * 30.0f, 0.0f };
			float h = compass_heading_deg(f, dec);

			assert(h >= 0.0f && h < 360.0f);
		}
	}
	/* A negative result must wrap up, not stay negative. */
	assert(NEAR(compass_heading_deg(north, -30.0f), 330.0f));
	assert(NEAR(compass_heading_deg(north, 390.0f), 30.0f));
	/* A hair west of north rounds onto 360.0f in float; that is 0. */
	{
		const float hair[3] = { 1e-6f, 10.0f, 0.0f };

		assert(compass_heading_deg(hair, 0.0f) < 360.0f);
	}
	assert(compass_heading_deg(NULL, 0.0f) == 0.0f);
}

static void test_log_prefix(void)
{
	struct compass_dev compass = fake_dev();
	struct compass_sample s;

	compass_err(&compass, "id at 0x%02x is 0x%02x", 0x2c, 0x81);
	assert(!strcmp(stub_log_last, "compass: fake: id at 0x2c is 0x81"));

	/* No arguments after the format must still compile and work. */
	compass_warn(&compass, "no arguments");
	assert(!strcmp(stub_log_last, "compass: fake: no arguments"));
	compass_info(&compass, "i");
	assert(!strcmp(stub_log_last, "compass: fake: i"));
	/* test/Makefile builds with CONFIG_COMPASS_DEBUG=1, so this one prints. */
	compass_dbg(&compass, "d");
	assert(!strcmp(stub_log_last, "compass: fake: d"));

	stub_tick = 42;
	fake_sample.field[0] = -12.5f;
	assert(compass_read(&compass, &s) == 0);
	compass_log_sample(&compass, &s);
	assert(!strcmp(stub_log_last, "compass: fake: t 42000 us  "
				      "x   -12.50  y    +2.00  z    +3.00 uT"));

	/*
	 * The timestamp is formatted by hand, because newlib-nano's printf has
	 * no %lld. Both ends of the range, and zero.
	 */
	s.t_us = 0;
	compass_log_sample(&compass, &s);
	assert(!strncmp(stub_log_last, "compass: fake: t 0 us  ", 23));
	s.t_us = INT64_MAX;
	compass_log_sample(&compass, &s);
	assert(strstr(stub_log_last, " t 9223372036854775807 us "));
	s.t_us = INT64_MIN;
	compass_log_sample(&compass, &s);
	assert(strstr(stub_log_last, " t -9223372036854775808 us "));
	s.t_us = -1234567;
	compass_log_sample(&compass, &s);
	assert(strstr(stub_log_last, " t -1234567 us "));
}

int main(void)
{
	test_read_stamps_and_identity();
	test_mount_rotation();
	test_errno_passthrough();
	test_heading();
	test_log_prefix();
	printf("all core checks passed\n");
	return 0;
}
