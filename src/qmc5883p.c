#include <errno.h>
#include <stdlib.h>

#include "compass_priv.h"

#if CONFIG_COMPASS_QMC5883P

#include "compass_log.h"
#include "qmc5883p.h"
#include "qmc5883p_math.h"
#include "qmc5883p_regs.h"

#define I2C_TIMEOUT_MS		100

/*
 * Self-test DRDY wait, by compass_tick_ms(). Continuous mode reaches 1500 Hz
 * (Table 2), and §7.3 step 6 allows 5 ms per measurement at this CTRL1;
 * 100 ms is 20x that, so only a part that never measures waits it out.
 */
#define DRDY_TIMEOUT_MS		100
#define DRDY_POLL_MS		1

struct qmc5883p {
	struct compass_i2c i2c;
	int range;	/* register codes, so the maths header indexes directly */
	int odr;
	int osr1;
	int osr2;
	int mode;	/* QMC5883P_MODE_NORMAL or _CONTINUOUS */
	bool overflow;	/* OVFL seen by a STATUS read, not yet reported */
};

static const struct compass_ops qmc5883p_ops;

/* Our state, or NULL if this compass_dev was not probed by this driver. */
static struct qmc5883p *to_qmc5883p(struct compass_dev *compass)
{
	if (!compass || compass->ops != &qmc5883p_ops)
		return NULL;
	return compass->priv;
}

/* ---------------------------------------------------------------- I2C ---- */

/*
 * §8.2.4: one register address written, then as many bytes as are ACKed --
 * address, repeated start, burst.
 */
static int qmc5883p_read_regs(struct qmc5883p *d, uint8_t reg, void *buf,
			      size_t len)
{
	return compass_i2c_read(&d->i2c, reg, buf, len, I2C_TIMEOUT_MS);
}

/* §8.2.3: register address, then one data byte. */
static int qmc5883p_write_reg(struct qmc5883p *d, uint8_t reg, uint8_t value)
{
	return compass_i2c_write(&d->i2c, reg, value, I2C_TIMEOUT_MS);
}

int qmc5883p_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		      size_t len)
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d || !buf || len == 0)
		return -EINVAL;
	return qmc5883p_read_regs(d, reg, buf, len);
}

int qmc5883p_reg_write(struct compass_dev *compass, uint8_t reg,
		       uint8_t value)
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d)
		return -EINVAL;
	return qmc5883p_write_reg(d, reg, value);
}

/*
 * One STATUS read. §9.2.2: reading it clears both DRDY and OVFL, so OVFL is
 * latched here until a read reports it -- otherwise a data_ready() poll
 * would silently swallow an overflow.
 */
static int qmc5883p_status(struct qmc5883p *d, uint8_t *status)
{
	int ret;

	ret = qmc5883p_read_regs(d, QMC5883P_STATUS, status, 1);
	if (ret)
		return ret;
	if (*status & QMC5883P_STATUS_OVFL)
		d->overflow = true;
	return 0;
}

/* ------------------------------------------------------------ config ----- */

static int qmc5883p_write_ctrl1(struct qmc5883p *d, int mode)
{
	return qmc5883p_write_reg(d, QMC5883P_CTRL1,
				  qmc5883p_ctrl1(d->osr2, d->osr1, d->odr,
						 mode));
}

/*
 * §9.2.3: suspend has to sit between any two of normal, single and
 * continuous, so every mode change passes through it. Writing suspend when
 * already suspended costs one transfer and keeps this the only path.
 */
static int qmc5883p_enter_mode(struct qmc5883p *d, int mode)
{
	int ret;

	ret = qmc5883p_write_reg(d, QMC5883P_CTRL1, QMC5883P_SUSPEND_CTRL1);
	if (ret || mode == QMC5883P_MODE_SUSPEND)
		return ret;
	return qmc5883p_write_ctrl1(d, mode);
}

/* Table 18 CTRL2 for the configured range, then the configured mode. */
static int qmc5883p_configure(struct qmc5883p *d)
{
	int ret;

	ret = qmc5883p_write_reg(d, QMC5883P_CTRL2, qmc5883p_ctrl2(d->range));
	if (ret)
		return ret;
	return qmc5883p_enter_mode(d, d->mode);
}

int qmc5883p_set_range(struct compass_dev *compass, enum qmc5883p_range range)
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d || qmc5883p_lsb_per_ut((int)range) <= 0.0f)
		return -EINVAL;

	d->range = (int)range;
	return qmc5883p_write_reg(d, QMC5883P_CTRL2, qmc5883p_ctrl2(d->range));
}

int qmc5883p_standby(struct compass_dev *compass, bool standby)
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d)
		return -EINVAL;
	return qmc5883p_enter_mode(d, standby ? QMC5883P_MODE_SUSPEND :
						d->mode);
}

/* -------------------------------------------------------------- read ----- */

static int qmc5883p_read_counts(struct qmc5883p *d, int16_t field[3])
{
	uint8_t b[QMC5883P_DATA_LEN];
	int ret;

	ret = qmc5883p_read_regs(d, QMC5883P_DATA_X_LSB, b, sizeof(b));
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		field[i] = qmc5883p_sample(&b[i * 2]);
	return 0;
}

/*
 * compass_ops.read, §7.5: STATUS, then the six data bytes. DRDY is not
 * waited for -- §9.2.1 keeps the last result in place until a new one
 * replaces it, so the latest reading is always there to take.
 */
static int qmc5883p_read(struct compass_dev *compass,
			 struct compass_sample *s)
{
	struct qmc5883p *d = compass->priv;
	uint8_t status;
	int16_t raw[3];
	int ret;

	ret = qmc5883p_status(d, &status);
	if (ret)
		return ret;
	ret = qmc5883p_read_counts(d, raw);
	if (ret)
		return ret;

	/* A clipped reading looks perfectly normal; refuse it. */
	if (d->overflow) {
		d->overflow = false;
		return -ERANGE;
	}

	for (int i = 0; i < 3; i++)
		s->field[i] = qmc5883p_ut(raw[i], d->range);
	return 0;
}

static const struct compass_ops qmc5883p_ops = {
	.read = qmc5883p_read,
};

/* ----------------------------------------------------------- extras ------ */

int qmc5883p_read_raw(struct compass_dev *compass, int16_t field[3])
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d || !field)
		return -EINVAL;
	return qmc5883p_read_counts(d, field);
}

int qmc5883p_data_ready(struct compass_dev *compass, bool *ready)
{
	struct qmc5883p *d = to_qmc5883p(compass);
	uint8_t status;
	int ret;

	if (!d || !ready)
		return -EINVAL;

	ret = qmc5883p_status(d, &status);
	if (ret)
		return ret;
	*ready = (status & QMC5883P_STATUS_DRDY) != 0;
	return 0;
}

/*
 * Polls until DRDY_TIMEOUT_MS has passed by compass_tick_ms(), DRDY_POLL_MS or
 * more between polls. The last read is at or after the deadline.
 */
static int qmc5883p_wait_drdy(struct qmc5883p *d)
{
	const uint32_t start = compass_tick_ms();
	uint8_t status;
	int ret;

	for (;;) {
		ret = qmc5883p_status(d, &status);
		if (ret)
			return ret;
		if (status & QMC5883P_STATUS_DRDY)
			return 0;
		if (compass_expired(start, DRDY_TIMEOUT_MS))
			return -ETIMEDOUT;
		compass_sleep_ms(DRDY_POLL_MS);
	}
}

/* §7.3, step by step. The restore at the end runs whatever happened. */
int qmc5883p_self_test(struct compass_dev *compass, int16_t delta[3])
{
	struct qmc5883p *d = to_qmc5883p(compass);
	int16_t before[3], after[3];
	int ret, restore;

	if (!d || !delta)
		return -EINVAL;

	/* §9.2.3: leave the current mode through suspend before step 2. */
	ret = qmc5883p_enter_mode(d, QMC5883P_MODE_SUSPEND);
	/*
	 * §7.3 starts from reset, CTRL2 = 0: both readings at 30 G. Step 5's
	 * 0x40 forces 30 G for the second one, so the first must be at 30 G
	 * too, or the delta compares two scales.
	 */
	if (!ret)
		ret = qmc5883p_write_reg(d, QMC5883P_CTRL2,
					 qmc5883p_ctrl2(QMC5883P_RANGE_3000UT));
	/*
	 * A DRDY left over from the previous mode would let step 3 pass at
	 * once and step 4 take a reading at the old range. Reading STATUS
	 * clears it (§9.2.2).
	 */
	if (!ret) {
		uint8_t status;

		ret = qmc5883p_status(d, &status);
	}
	if (!ret)		/* step 1 */
		ret = qmc5883p_write_reg(d, QMC5883P_AXIS_SIGN,
					 QMC5883P_AXIS_SIGN_VALUE);
	if (!ret)		/* step 2: continuous mode */
		ret = qmc5883p_write_reg(d, QMC5883P_CTRL1,
					 QMC5883P_SELF_TEST_CTRL1);
	if (!ret)		/* step 3 */
		ret = qmc5883p_wait_drdy(d);
	if (!ret)		/* step 4 */
		ret = qmc5883p_read_counts(d, before);
	if (!ret)		/* step 5: stimulus on */
		ret = qmc5883p_write_reg(d, QMC5883P_CTRL2,
					 QMC5883P_SELF_TEST_CTRL2);
	if (!ret) {		/* step 6 */
		compass_sleep_ms(QMC5883P_SELF_TEST_WAIT_MS);
		ret = qmc5883p_read_counts(d, after);	/* step 7 */
	}
	if (!ret)		/* step 8 */
		qmc5883p_self_test_delta(before, after, delta);

	/*
	 * §6.2.3: the part drops to suspend by itself once the self-test data
	 * is in; enter_mode writes suspend anyway, in case it has not. Step 5
	 * also moved RNG to 30 G, so CTRL2 goes back too.
	 *
	 * The stimulated reading's DRDY, and any OVFL it raised, are still set;
	 * one STATUS read clears both (§9.2.2), so data_ready() waits for a
	 * real measurement and the stimulus's overflow is not the caller's.
	 */
	restore = qmc5883p_configure(d);
	if (!restore) {
		uint8_t status;

		restore = qmc5883p_status(d, &status);
	}
	d->overflow = false;
	if (ret)
		compass_err(compass, "self-test failed (%d)", ret);
	return ret ? ret : restore;
}

/* ------------------------------------------------------- probe/remove ---- */

int qmc5883p_probe(struct compass_dev *compass, compass_i2c_bus bus,
		   const struct qmc5883p_config *cfg)
{
	static const struct qmc5883p_config defaults;	/* all zero: defaults */
	struct qmc5883p *d;
	uint8_t id = 0;
	int ret;

	if (!compass || !bus)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;
	if (!cfg)
		cfg = &defaults;

	compass->name = "qmc5883p";

	if (qmc5883p_lsb_per_ut((int)cfg->range) <= 0.0f ||
	    qmc5883p_odr_hz((int)cfg->odr) <= 0.0f ||
	    qmc5883p_osr1_ratio((int)cfg->osr1) == 0 ||
	    qmc5883p_osr2_ratio((int)cfg->osr2) == 0)
		return -EINVAL;

	/*
	 * Anything there at all? See compass_i2c_probe() for why not the first
	 * read. -ENXIO means "not on this bus" and stays at debug level, so
	 * examples/gy271 can try the other chip without a line of noise.
	 */
	ret = compass_i2c_probe(bus, QMC5883P_ADDR, I2C_TIMEOUT_MS);
	if (ret == -ENXIO) {
		compass_dbg(compass, "nothing answered at 0x%02x (%d)",
			    QMC5883P_ADDR, ret);
		return ret;
	}
	if (ret) {
		compass_err(compass, "presence check at 0x%02x failed (%d)",
			    QMC5883P_ADDR, ret);
		return ret;
	}

	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;
	d->range = (int)cfg->range;
	d->odr = (int)cfg->odr;
	d->osr1 = (int)cfg->osr1;
	d->osr2 = (int)cfg->osr2;
	d->mode = cfg->continuous ? QMC5883P_MODE_CONTINUOUS :
				    QMC5883P_MODE_NORMAL;

	ret = compass_i2c_attach(&d->i2c, bus, QMC5883P_ADDR,
				 compass_config_scl_hz(cfg));
	if (ret) {
		compass_err(compass, "cannot add 0x%02x to the bus (%d)",
			    QMC5883P_ADDR, ret);
		free(d);
		return ret;
	}

	compass->ops = &qmc5883p_ops;
	compass->priv = d;

	ret = qmc5883p_read_regs(d, QMC5883P_CHIP_ID, &id, 1);
	if (ret)
		goto fail;
	if (id != QMC5883P_CHIP_ID_VALUE) {
		compass_err(compass, "CHIPID at 0x%02x is 0x%02x, not 0x%02x",
			    QMC5883P_ADDR, id, QMC5883P_CHIP_ID_VALUE);
		ret = -ENODEV;
		goto fail;
	}

	/* §7.6 soft reset, then Table 7's power-on time; see the regs header. */
	ret = qmc5883p_write_reg(d, QMC5883P_CTRL2, QMC5883P_SOFT_RST);
	if (ret)
		goto fail;
	compass_sleep_ms((QMC5883P_RESET_WAIT_US + 999) / 1000);

	/* §7.1/§7.2 step 1, then CTRL2 and CTRL1 in the same order they use. */
	ret = qmc5883p_write_reg(d, QMC5883P_AXIS_SIGN,
				 QMC5883P_AXIS_SIGN_VALUE);
	if (!ret)
		ret = qmc5883p_configure(d);
	if (ret)
		goto fail;

	compass_info(compass, "at 0x%02x: +/-%.0f uT, %s, ODR %.0f Hz, OSR1 %d, "
		     "OSR2 %d", QMC5883P_ADDR, (double)qmc5883p_fs_ut(d->range),
		     d->mode == QMC5883P_MODE_CONTINUOUS ?
			     "continuous (ODR unused)" : "normal",
		     (double)qmc5883p_odr_hz(d->odr),
		     qmc5883p_osr1_ratio(d->osr1),
		     qmc5883p_osr2_ratio(d->osr2));
	return 0;

fail:
	compass->ops = NULL;
	compass->priv = NULL;
	compass_i2c_detach(&d->i2c);
	free(d);
	return ret;
}

void qmc5883p_remove(struct compass_dev *compass)
{
	struct qmc5883p *d = to_qmc5883p(compass);

	if (!d)
		return;

	qmc5883p_enter_mode(d, QMC5883P_MODE_SUSPEND);
	compass_i2c_detach(&d->i2c);
	free(d);
	compass->ops = NULL;
	compass->priv = NULL;
}

#endif /* CONFIG_COMPASS_QMC5883P */
