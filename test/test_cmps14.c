/*
 * Host-side check of the CMPS14 serial driver against a fake module behind
 * the fake HAL UART.
 *
 * The fake answers the p.6 command table of docs/cmps14.pdf byte by byte:
 * each command queues its answer, which the driver's receive then takes.
 * The p.7 sequences answer 0x55 per byte only in order; out of order, the
 * module stays silent. Bytes left unread sit in the fake's receive queue,
 * which is what the port's stale-input drop has to clear.
 */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stm32_hal_stub.h"

#include "cmps14.h"
#include "cmps14_proto.h"
#include "cmps14_math.h"
#include "compass.h"

static UART_HandleTypeDef uart;

/* The module. */
static uint8_t version;
static int16_t mag[3];
static uint16_t bearing;
static uint8_t cal_state;
static bool silent;		/* nothing on the line */
static bool bad_ok;		/* sequences answer 0xAA instead of 0x55 */
static bool answer_setup;	/* the setup byte gets setup_reply */
static uint8_t setup_reply;
static bool late_setup;		/* ... but only once someone waits for it */
static int pending = -1;	/* a late byte not yet on the line */
static int config_step, store_step, erase_step;	/* bytes matched so far */
static int setup_byte;		/* -1 until one arrives */
static int stored, erased;

/* The line, module to driver. */
static uint8_t queue[64];
static size_t queued;
static HAL_StatusTypeDef send_status;
static int allocs, frees;
static long slept_ms;

static void module_power_on(void)
{
	version = 7;
	memset(mag, 0, sizeof(mag));
	bearing = 0;
	cal_state = 0;
	silent = bad_ok = answer_setup = late_setup = false;
	setup_reply = CMPS14_OK;
	pending = -1;
	config_step = store_step = erase_step = 0;
	setup_byte = -1;
	stored = erased = 0;
	queued = 0;
	send_status = HAL_OK;
	allocs = frees = 0;
	slept_ms = 0;
}

static void answer(uint8_t b)
{
	assert(queued < sizeof(queue));
	queue[queued++] = b;
}

static void answer16(uint16_t v)	/* p.6: high byte first */
{
	answer((uint8_t)(v >> 8));
	answer((uint8_t)v);
}

/* One step of a three-byte sequence: 0x55 when in order, else silence. */
static bool sequence(const uint8_t seq[3], int *step, uint8_t b)
{
	if (b == seq[*step] || b == seq[0]) {
		*step = b == seq[*step] ? *step + 1 : 1;
		answer(bad_ok ? 0xAA : CMPS14_OK);
		return true;
	}
	*step = 0;
	return false;
}

static void module_receive(uint8_t b)
{
	static const uint8_t config[3] = { 0x98, 0x95, 0x99 };
	static const uint8_t store[3] = { 0xF0, 0xF5, 0xF6 };
	static const uint8_t erase[3] = { 0xE0, 0xE5, 0xE2 };

	if (silent)
		return;
	if (config_step == 3) {		/* the setup byte, p.7 */
		setup_byte = b;
		config_step = 0;
		if (late_setup)
			pending = setup_reply;
		else if (answer_setup)
			answer(setup_reply);
		return;
	}
	switch (b) {
	case 0x11:
		answer(version);
		return;
	case 0x13:
		answer16(bearing);
		return;
	case 0x19:
		for (int i = 0; i < 3; i++)
			answer16((uint16_t)mag[i]);
		return;
	case 0x24:
		answer(cal_state);
		return;
	}
	if (sequence(config, &config_step, b))
		return;
	if (sequence(store, &store_step, b)) {
		if (store_step == 3) {
			stored++;
			store_step = 0;
		}
		return;
	}
	if (sequence(erase, &erase_step, b)) {
		if (erase_step == 3) {
			erased++;
			erase_step = 0;
		}
		return;
	}
}

HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *h, uint8_t *data,
				    uint16_t size, uint32_t timeout)
{
	assert(h == &uart && size >= 1 && timeout > 0);
	if (send_status != HAL_OK)
		return send_status;
	for (int i = 0; i < size; i++)
		module_receive(data[i]);
	return HAL_OK;
}

/*
 * Blocking receive: all size bytes, or HAL_TIMEOUT. A late byte arrives
 * while someone is waiting, ahead of anything queued after it was due.
 */
HAL_StatusTypeDef HAL_UART_Receive(UART_HandleTypeDef *h, uint8_t *data,
				   uint16_t size, uint32_t timeout)
{
	size_t n;

	assert(h == &uart && timeout > 0);
	if (pending >= 0) {
		assert(queued < sizeof(queue));
		memmove(queue + 1, queue, queued++);
		queue[0] = (uint8_t)pending;
		pending = -1;
	}
	n = size < queued ? size : queued;
	memcpy(data, queue, n);
	memmove(queue, queue + n, queued - n);
	queued -= n;
	return n == size ? HAL_OK : HAL_TIMEOUT;
}

int stub_uart_get_flag(UART_HandleTypeDef *h, uint32_t flag)
{
	assert(h == &uart && flag == UART_FLAG_RXNE);
	return queued > 0;
}

void stub_uart_clear_ore(UART_HandleTypeDef *h)
{
	assert(h == &uart);
}

void stub_uart_flush(UART_HandleTypeDef *h)
{
	assert(h == &uart && queued > 0);
	memmove(queue, queue + 1, --queued);
}

void HAL_Delay(uint32_t ms)
{
	stub_tick += ms + 1;
	slept_ms += (long)ms;
}

/* Counts the driver's calloc() and free() calls; see test/Makefile. */
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
	allocs++;
	return __real_calloc(n, size);
}
void __real_free(void *p);
void __wrap_free(void *p);
void __wrap_free(void *p)
{
	frees++;
	__real_free(p);
}

static int near(float a, float b)
{
	return fabsf(a - b) < 1e-3f;
}

static struct compass_dev open_ok(void)
{
	struct compass_dev compass = { 0 };

	module_power_on();
	assert(cmps14_probe(&compass, &uart) == 0);
	return compass;
}

/* -------------------------------------------------------------- tests -- */

static void test_probe(void)
{
	struct compass_dev compass = { 0 };
	uint8_t v = 0;

	compass = open_ok();
	assert(strcmp(compass.name, "cmps14") == 0 && allocs == 1);
	assert(strstr(stub_log_last, "on UART: software version 7"));
	assert(cmps14_version(&compass, &v) == 0 && v == 7);
	assert(cmps14_probe(&compass, &uart) == -EBUSY);
	cmps14_remove(&compass);
	assert(!compass.ops && !compass.priv && frees == 1);

	/* Nothing on the line: -ENXIO, nothing allocated. */
	module_power_on();
	silent = true;
	assert(cmps14_probe(&compass, &uart) == -ENXIO);
	assert(allocs == 0 && !compass.ops && !compass.priv);

	/* A failing UART keeps its errno. */
	module_power_on();
	send_status = HAL_ERROR;
	assert(cmps14_probe(&compass, &uart) == -EIO);
	assert(allocs == 0);

	/* A stale byte on the line is not taken for the version. */
	module_power_on();
	answer(0x99);
	assert(cmps14_probe(&compass, &uart) == 0);
	assert(cmps14_version(&compass, &v) == 0 && v == 7);
	cmps14_remove(&compass);

	assert(cmps14_probe(NULL, &uart) == -EINVAL);
	assert(cmps14_probe(&compass, NULL) == -EINVAL);
}

static void test_read(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s;
	int16_t raw[3];

	/* Q point 4: 16 counts per uT (p.3, p.6). */
	mag[0] = 30 * 16;
	mag[1] = -5 * 16;
	mag[2] = 8;
	assert(compass_read(&compass, &s) == 0);
	assert(near(s.field[0], 30.0f) && near(s.field[1], -5.0f) &&
	       near(s.field[2], 0.5f));
	assert(cmps14_read_raw(&compass, raw) == 0);
	assert(raw[0] == 480 && raw[1] == -80 && raw[2] == 8);

	/* A module gone quiet is a timeout, and the sample is untouched. */
	silent = true;
	s.field[0] = 42.0f;
	assert(compass_read(&compass, &s) == -ETIMEDOUT);
	assert(s.field[0] == 42.0f);
	silent = false;
	cmps14_remove(&compass);
}

static void test_bearing_and_state(void)
{
	struct compass_dev compass = open_ok();
	uint8_t state = 0;
	float deg = -1.0f;

	bearing = 3599;
	assert(cmps14_bearing_deg(&compass, &deg) == 0 && near(deg, 359.9f));
	bearing = 1800;
	assert(cmps14_bearing_deg(&compass, &deg) == 0 && near(deg, 180.0f));
	bearing = 0;
	assert(cmps14_bearing_deg(&compass, &deg) == 0 && deg == 0.0f);

	cal_state = 0xC3;
	assert(cmps14_calibration_state(&compass, &state) == 0);
	assert(state == 0xC3);
	cmps14_remove(&compass);
}

static void test_calibration(void)
{
	struct compass_dev compass = open_ok();
	struct compass_sample s;
	float deg;

	/* p.7: 0x98 0x95 0x99, each 0x55, then 1xx?x??? with bit 7 set. */
	assert(cmps14_set_calibration(&compass, CMPS14_CAL_MAG |
				      CMPS14_CAL_ACCEL |
				      CMPS14_CAL_AUTOSAVE) == 0);
	assert(setup_byte == 0x93);
	assert(cmps14_set_calibration(&compass, 0) == 0);
	assert(setup_byte == 0x80);

	/* Undefined bits, and bit 7 itself, are not the caller's. */
	setup_byte = -1;
	assert(cmps14_set_calibration(&compass, 0x08) == -EINVAL);
	assert(cmps14_set_calibration(&compass, 0x20) == -EINVAL);
	assert(cmps14_set_calibration(&compass, 0x80) == -EINVAL);
	assert(setup_byte == -1 && config_step == 0);

	/* The setup byte's answer: at once, or silence, both fine. */
	answer_setup = true;
	assert(cmps14_set_calibration(&compass, CMPS14_CAL_MAG) == 0);
	assert(queued == 0);
	answer_setup = false;
	assert(cmps14_set_calibration(&compass, CMPS14_CAL_MAG) == 0);

	/*
	 * Late: the 0x55 only turns up while someone waits. Unread, it would
	 * be the first byte of the next answer and shift X, Y and Z.
	 */
	late_setup = true;
	assert(cmps14_set_calibration(&compass, CMPS14_CAL_MAG) == 0);
	assert(pending == -1 && queued == 0);
	mag[0] = 30 * 16;
	mag[1] = -5 * 16;
	mag[2] = 8;
	assert(compass_read(&compass, &s) == 0);
	assert(near(s.field[0], 30.0f) && near(s.field[1], -5.0f) &&
	       near(s.field[2], 0.5f));
	late_setup = false;

	/* Any other answer to the setup byte is -EIO. */
	answer_setup = true;
	setup_reply = 0xAA;
	assert(cmps14_set_calibration(&compass, CMPS14_CAL_MAG) == -EIO);
	answer_setup = false;
	setup_reply = CMPS14_OK;
	bearing = 900;
	assert(cmps14_bearing_deg(&compass, &deg) == 0 && near(deg, 90.0f));

	assert(cmps14_store_calibration(&compass) == 0 && stored == 1);

	/* Erase resets the BNO080, then waits p.7's 300 ms. */
	slept_ms = 0;
	assert(cmps14_erase_calibration(&compass) == 0 && erased == 1);
	assert(slept_ms >= CMPS14_ERASE_WAIT_MS);

	/* An answer other than 0x55 is -EIO, and the sequence stops. */
	bad_ok = true;
	assert(cmps14_store_calibration(&compass) == -EIO);
	assert(stored == 1);
	bad_ok = false;

	/* Silence mid-sequence is a timeout. */
	silent = true;
	assert(cmps14_erase_calibration(&compass) == -ETIMEDOUT);
	silent = false;
	cmps14_remove(&compass);
}

static void test_extras(void)
{
	struct compass_dev compass = open_ok(), other = { 0 };
	uint8_t b = 0;
	float deg;

	/* The escape hatch: any command, any answer length. */
	assert(cmps14_command(&compass, 0x11, &b, 1) == 0 && b == 7);
	assert(cmps14_command(&compass, 0x11, NULL, 1) == -EINVAL);
	assert(cmps14_command(&compass, 0x7F, NULL, 0) == 0);

	/* Someone else's device, or none. */
	assert(cmps14_bearing_deg(&other, &deg) == -EINVAL);
	assert(cmps14_store_calibration(NULL) == -EINVAL);
	assert(cmps14_bearing_deg(&compass, NULL) == -EINVAL);
	assert(cmps14_version(&compass, NULL) == -EINVAL);
	cmps14_remove(&other);		/* safe on an unprobed device */

	cmps14_remove(&compass);
	assert(cmps14_read_raw(&compass, (int16_t[3]){ 0 }) == -EINVAL);
	assert(allocs == frees);
}

int main(void)
{
	test_probe();
	test_read();
	test_bearing_and_state();
	test_calibration();
	test_extras();
	printf("all cmps14 driver checks passed\n");
	return 0;
}
