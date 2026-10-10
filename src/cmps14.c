#include <errno.h>
#include <stdlib.h>

#include "compass_priv.h"

#if CONFIG_COMPASS_CMPS14

#include "cmps14.h"
#include "cmps14_cmds.h"
#include "cmps14_math.h"
#include "compass_log.h"

/*
 * The document gives no answer time. At 9600 bps a six-byte answer is about
 * 6 ms on the wire, so 100 ms leaves the module's processor ample room.
 */
#define UART_TIMEOUT_MS		100

#define CAL_ENABLE_BITS		(CMPS14_CAL_MAG | CMPS14_CAL_ACCEL | \
				 CMPS14_CAL_GYRO | CMPS14_CAL_AUTOSAVE)

struct cmps14 {
	struct compass_bus bus;
	uint8_t version;
};

static const struct compass_ops cmps14_ops;

/* Our state, or NULL if this compass_dev was not probed by this driver. */
static struct cmps14 *to_cmps14(struct compass_dev *compass)
{
	if (!compass || compass->ops != &cmps14_ops)
		return NULL;
	return compass->priv;
}

/* --------------------------------------------------------- commands ---- */

static int cmps14_ask(struct compass_bus *bus, uint8_t cmd, void *answer,
		      size_t len)
{
	return bus->ops->transfer(bus, cmd, NULL, 0, answer, len);
}

/* One step of a p.7 sequence: the byte goes out, 0x55 must come back. */
static int cmps14_step(struct cmps14 *d, uint8_t cmd)
{
	uint8_t ok = 0;
	int ret;

	ret = cmps14_ask(&d->bus, cmd, &ok, 1);
	if (ret)
		return ret;
	return ok == CMPS14_OK ? 0 : -EIO;
}

static int cmps14_sequence(struct cmps14 *d, const uint8_t cmd[3])
{
	int ret;

	for (int i = 0; i < 3; i++) {
		ret = cmps14_step(d, cmd[i]);
		if (ret)
			return ret;
	}
	return 0;
}

int cmps14_command(struct compass_dev *compass, uint8_t cmd, void *answer,
		   size_t len)
{
	struct cmps14 *d = to_cmps14(compass);

	if (!d || (len && !answer))
		return -EINVAL;
	return cmps14_ask(&d->bus, cmd, answer, len);
}

/* -------------------------------------------------------------- read ----- */

static int cmps14_read_counts(struct cmps14 *d, int16_t field[3])
{
	uint8_t b[CMPS14_MAGNETOMETER_LEN];
	int ret;

	ret = cmps14_ask(&d->bus, CMPS14_GET_MAGNETOMETER, b, sizeof(b));
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		field[i] = cmps14_sample(&b[i * 2]);
	return 0;
}

/*
 * compass_ops.read: one GET MAGNETOMETER. The module reports no overflow and
 * the document gives no range, so there is no -ERANGE here.
 */
static int cmps14_read(struct compass_dev *compass, struct compass_sample *s)
{
	struct cmps14 *d = compass->priv;
	int16_t raw[3];
	int ret;

	ret = cmps14_read_counts(d, raw);
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		s->field[i] = cmps14_ut(raw[i]);
	return 0;
}

static const struct compass_ops cmps14_ops = {
	.read = cmps14_read,
};

/* ----------------------------------------------------------- extras ------ */

int cmps14_version(struct compass_dev *compass, uint8_t *version)
{
	struct cmps14 *d = to_cmps14(compass);

	if (!d || !version)
		return -EINVAL;
	*version = d->version;
	return 0;
}

int cmps14_read_raw(struct compass_dev *compass, int16_t field[3])
{
	struct cmps14 *d = to_cmps14(compass);

	if (!d || !field)
		return -EINVAL;
	return cmps14_read_counts(d, field);
}

int cmps14_bearing_deg(struct compass_dev *compass, float *degrees)
{
	struct cmps14 *d = to_cmps14(compass);
	uint8_t b[2];
	int ret;

	if (!d || !degrees)
		return -EINVAL;

	ret = cmps14_ask(&d->bus, CMPS14_GET_BEARING_16, b, sizeof(b));
	if (ret)
		return ret;
	*degrees = cmps14_bearing((uint16_t)cmps14_sample(b));
	return 0;
}

int cmps14_calibration_state(struct compass_dev *compass, uint8_t *state)
{
	struct cmps14 *d = to_cmps14(compass);

	if (!d || !state)
		return -EINVAL;
	return cmps14_ask(&d->bus, CMPS14_GET_CALIBRATION_STATE, state, 1);
}

/*
 * p.7 gives the setup byte no answer. The module may still send 0x55 for
 * it, and late: if nothing read it, it would land after the next command
 * went out and become that answer's first byte. So wait for one byte here:
 * 0x55 or silence are both success, anything else is -EIO.
 */
int cmps14_set_calibration(struct compass_dev *compass, uint8_t enable)
{
	static const uint8_t unlock[3] = {
		CMPS14_CAL_CONFIG_1, CMPS14_CAL_CONFIG_2, CMPS14_CAL_CONFIG_3,
	};
	struct cmps14 *d = to_cmps14(compass);
	uint8_t ok = 0;
	int ret;

	if (!d || (enable & ~CAL_ENABLE_BITS))
		return -EINVAL;

	ret = cmps14_sequence(d, unlock);
	if (ret)
		return ret;
	ret = cmps14_ask(&d->bus, (uint8_t)(CMPS14_SETUP_ALWAYS | enable),
			 &ok, 1);
	if (ret == -ETIMEDOUT)
		return 0;
	if (ret)
		return ret;
	return ok == CMPS14_OK ? 0 : -EIO;
}

int cmps14_store_calibration(struct compass_dev *compass)
{
	static const uint8_t store[3] = {
		CMPS14_STORE_1, CMPS14_STORE_2, CMPS14_STORE_3,
	};
	struct cmps14 *d = to_cmps14(compass);

	if (!d)
		return -EINVAL;
	return cmps14_sequence(d, store);
}

int cmps14_erase_calibration(struct compass_dev *compass)
{
	static const uint8_t erase[3] = {
		CMPS14_ERASE_1, CMPS14_ERASE_2, CMPS14_ERASE_3,
	};
	struct cmps14 *d = to_cmps14(compass);
	int ret;

	if (!d)
		return -EINVAL;
	ret = cmps14_sequence(d, erase);
	if (ret)
		return ret;
	compass_sleep_ms(CMPS14_ERASE_WAIT_MS);
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

int cmps14_probe(struct compass_dev *compass, compass_uart_port uart)
{
	struct compass_bus bus;
	struct cmps14 *d;
	uint8_t version = 0;
	int ret;

	if (!compass)
		return -EINVAL;
#ifndef ESP_PLATFORM
	if (!uart)
		return -EINVAL;
#endif
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;

	compass->name = "cmps14";

	ret = compass_uart_attach(&bus, uart, UART_TIMEOUT_MS);
	if (ret) {
		compass_err(compass, "cannot use the UART (%d)", ret);
		return ret;
	}

	/*
	 * Anything there at all? Silence is -ENXIO, at debug level like a
	 * missing I2C ACK, and decided on a bus on the stack, before anything
	 * is allocated.
	 */
	ret = cmps14_ask(&bus, CMPS14_GET_VERSION, &version, 1);
	if (ret == -ETIMEDOUT) {
		compass_dbg(compass, "nothing answered on the UART (%d)", ret);
		compass_bus_detach(&bus);
		return -ENXIO;
	}
	if (ret) {
		compass_err(compass, "version request failed (%d)", ret);
		compass_bus_detach(&bus);
		return ret;
	}

	d = calloc(1, sizeof(*d));
	if (!d) {
		compass_bus_detach(&bus);
		return -ENOMEM;
	}
	d->bus = bus;
	d->version = version;

	compass->ops = &cmps14_ops;
	compass->priv = d;
	compass_info(compass, "on UART: software version %u",
		     (unsigned int)version);
	return 0;
}

void cmps14_remove(struct compass_dev *compass)
{
	struct cmps14 *d = to_cmps14(compass);

	if (!d)
		return;

	compass_bus_detach(&d->bus);
	free(d);
	compass->ops = NULL;
	compass->priv = NULL;
}

#endif /* CONFIG_COMPASS_CMPS14 */
