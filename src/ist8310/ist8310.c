#include <errno.h>
#include <stdlib.h>

#include "compass_priv.h"

#if CONFIG_COMPASS_IST8310

#include "compass_log.h"
#include "ist8310.h"
#include "ist8310_math.h"
#include "ist8310_regs.h"

#define I2C_TIMEOUT_MS		100

/*
 * A measurement takes 5 ms, 6 ms with 16x averaging (§3.1.1, §3.1.2). One
 * started that long ago and still not ready never will be -- a lost trigger,
 * or the data taken through ist8310_reg_read() -- so the next read starts
 * another. 100 ms is 16x the slowest.
 */
#define MEASURE_TIMEOUT_MS	100

struct ist8310 {
	struct compass_bus bus;
	int avg_xz;		/* register codes */
	int avg_y;
	bool measuring;		/* a trigger was accepted ... */
	uint32_t started;	/* ... at this compass_tick_ms() */
};

static const struct compass_ops ist8310_ops;

/* Our state, or NULL if this compass_dev was not probed by this driver. */
static struct ist8310 *to_ist8310(struct compass_dev *compass)
{
	if (!compass || compass->ops != &ist8310_ops)
		return NULL;
	return compass->priv;
}

/* ---------------------------------------------------------------- I2C ---- */

/* §6.2: register address, repeated start, then as many bytes as are ACKed. */
static int ist8310_read_regs(struct ist8310 *d, uint8_t reg, void *buf,
			     size_t len)
{
	return compass_reg_read(&d->bus, reg, buf, len);
}

/* §6.3: register address, then one data byte. */
static int ist8310_write_reg(struct ist8310 *d, uint8_t reg, uint8_t value)
{
	return compass_reg_write(&d->bus, reg, value);
}

int ist8310_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		     size_t len)
{
	struct ist8310 *d = to_ist8310(compass);

	if (!d || !buf || len == 0)
		return -EINVAL;
	return ist8310_read_regs(d, reg, buf, len);
}

int ist8310_reg_write(struct compass_dev *compass, uint8_t reg, uint8_t value)
{
	struct ist8310 *d = to_ist8310(compass);

	if (!d)
		return -EINVAL;
	return ist8310_write_reg(d, reg, value);
}

/* -------------------------------------------------------------- read ----- */

/* §3.1.2: one single measurement, after which CNTL1 returns to 0000. */
static int ist8310_start(struct ist8310 *d)
{
	int ret;

	ret = ist8310_write_reg(d, IST8310_CNTL1, IST8310_MODE_SINGLE);
	d->measuring = ret == 0;
	d->started = compass_tick_ms();
	return ret;
}

/*
 * §3.4: STAT1, then the six data bytes, which clears DRDY. Then the next
 * measurement, so it is under way while the caller works on this one. A
 * failed trigger does not cost this sample: the next read sees measuring
 * false and tries again.
 *
 * Nothing ready is -ENODATA, and starts a measurement if none is running.
 */
static int ist8310_take(struct ist8310 *d, int16_t field[3])
{
	uint8_t status, b[IST8310_DATA_LEN];
	int ret;

	ret = ist8310_read_regs(d, IST8310_STAT1, &status, 1);
	if (ret)
		return ret;

	if (!(status & IST8310_STAT1_DRDY)) {
		if (!d->measuring ||
		    compass_expired(d->started, MEASURE_TIMEOUT_MS)) {
			ret = ist8310_start(d);
			if (ret)
				return ret;
		}
		return -ENODATA;
	}

	ret = ist8310_read_regs(d, IST8310_DATA_X_LSB, b, sizeof(b));
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		field[i] = ist8310_sample(&b[i * 2]);

	ist8310_start(d);
	return 0;
}

static int ist8310_read(struct compass_dev *compass, struct compass_sample *s)
{
	struct ist8310 *d = compass->priv;
	int16_t raw[3];
	int ret;

	ret = ist8310_take(d, raw);
	if (ret)
		return ret;

	/* No overflow flag on this part; §4.4's range is the limit. */
	if (ist8310_clipped(raw))
		return -ERANGE;

	for (int i = 0; i < 3; i++)
		s->field[i] = ist8310_ut(raw[i]);
	return 0;
}

static const struct compass_ops ist8310_ops = {
	.read = ist8310_read,
};

/* ----------------------------------------------------------- extras ------ */

int ist8310_read_raw(struct compass_dev *compass, int16_t field[3])
{
	struct ist8310 *d = to_ist8310(compass);

	if (!d || !field)
		return -EINVAL;
	return ist8310_take(d, field);
}

int ist8310_data_ready(struct compass_dev *compass, bool *ready)
{
	struct ist8310 *d = to_ist8310(compass);
	uint8_t status;
	int ret;

	if (!d || !ready)
		return -EINVAL;

	ret = ist8310_read_regs(d, IST8310_STAT1, &status, 1);
	if (ret)
		return ret;
	*ready = (status & IST8310_STAT1_DRDY) != 0;
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

int ist8310_probe(struct compass_dev *compass, compass_i2c_bus bus,
		  const struct ist8310_config *cfg)
{
	static const struct ist8310_config defaults;	/* all zero: defaults */
	struct ist8310 *d;
	uint8_t address, id = 0;
	int ret;

	if (!compass || !bus)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;
	if (!cfg)
		cfg = &defaults;

	compass->name = "ist8310";

	address = cfg->address ? cfg->address : IST8310_ADDR_HL;
	if (address < IST8310_ADDR_LL || address > IST8310_ADDR_HH ||
	    ist8310_avg_samples((int)cfg->avg_xz) == 0 ||
	    ist8310_avg_samples((int)cfg->avg_y) == 0)
		return -EINVAL;

	/*
	 * Anything there at all? See compass_i2c_probe() for why not the first
	 * read. -ENXIO means "not on this bus" and stays at debug level, so
	 * a caller can try the other addresses without a line of noise.
	 */
	ret = compass_i2c_probe(bus, address, I2C_TIMEOUT_MS);
	if (ret == -ENXIO) {
		compass_dbg(compass, "nothing answered at 0x%02x (%d)",
			    address, ret);
		return ret;
	}
	if (ret) {
		compass_err(compass, "presence check at 0x%02x failed (%d)",
			    address, ret);
		return ret;
	}

	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;
	d->avg_xz = (int)cfg->avg_xz;
	d->avg_y = (int)cfg->avg_y;

	ret = compass_i2c_attach(&d->bus, bus, address,
				 compass_config_scl_hz(cfg), I2C_TIMEOUT_MS);
	if (ret) {
		compass_err(compass, "cannot add 0x%02x to the bus (%d)",
			    address, ret);
		free(d);
		return ret;
	}

	compass->ops = &ist8310_ops;
	compass->priv = d;

	ret = ist8310_read_regs(d, IST8310_WAI, &id, 1);
	if (ret)
		goto fail;
	if (id != IST8310_WAI_VALUE) {
		compass_err(compass, "WAI at 0x%02x is 0x%02x, not 0x%02x",
			    address, id, IST8310_WAI_VALUE);
		ret = -ENODEV;
		goto fail;
	}

	/* §6.4.7 soft reset, then §4.5's POR time; see the regs header. */
	ret = ist8310_write_reg(d, IST8310_CNTL2, IST8310_CNTL2_SRST);
	if (ret)
		goto fail;
	compass_sleep_ms(IST8310_RESET_WAIT_MS);

	/* §3.1.1's initial settings, then the first measurement. */
	ret = ist8310_write_reg(d, IST8310_AVGCNTL,
				ist8310_avgcntl(d->avg_xz, d->avg_y));
	if (!ret)
		ret = ist8310_write_reg(d, IST8310_PDCNTL,
					IST8310_PDCNTL_NORMAL);
	if (!ret)
		ret = ist8310_start(d);
	if (ret)
		goto fail;

	compass_info(compass, "at 0x%02x: averaging %d (X, Z) and %d (Y)",
		     address, ist8310_avg_samples(d->avg_xz),
		     ist8310_avg_samples(d->avg_y));
	return 0;

fail:
	compass->ops = NULL;
	compass->priv = NULL;
	compass_bus_detach(&d->bus);
	free(d);
	return ret;
}

void ist8310_remove(struct compass_dev *compass)
{
	struct ist8310 *d = to_ist8310(compass);

	if (!d)
		return;

	ist8310_write_reg(d, IST8310_CNTL1, IST8310_MODE_STANDBY);
	compass_bus_detach(&d->bus);
	free(d);
	compass->ops = NULL;
	compass->priv = NULL;
}

#endif /* CONFIG_COMPASS_IST8310 */
