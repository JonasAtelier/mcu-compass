#include <errno.h>
#include <stdlib.h>

#include "compass_priv.h"

#if CONFIG_COMPASS_QMC5883L

#include "compass_log.h"
#include "qmc5883l.h"
#include "qmc5883l_math.h"
#include "qmc5883l_regs.h"

#define I2C_TIMEOUT_MS		100

struct qmc5883l {
	struct compass_bus bus;
	int range;	/* register codes, so the maths header indexes directly */
	int odr;
	int osr;
};

static const struct compass_ops qmc5883l_ops;

/* Our state, or NULL if this compass_dev was not probed by this driver. */
static struct qmc5883l *to_qmc5883l(struct compass_dev *compass)
{
	if (!compass || compass->ops != &qmc5883l_ops)
		return NULL;
	return compass->priv;
}

/* ---------------------------------------------------------------- I2C ---- */

/* Register address, repeated start, then the burst. */
static int qmc5883l_read_regs(struct qmc5883l *d, uint8_t reg, void *buf,
			      size_t len)
{
	return compass_reg_read(&d->bus, reg, buf, len);
}

static int qmc5883l_write_reg(struct qmc5883l *d, uint8_t reg, uint8_t value)
{
	return compass_reg_write(&d->bus, reg, value);
}

int qmc5883l_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		      size_t len)
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d || !buf || len == 0)
		return -EINVAL;
	return qmc5883l_read_regs(d, reg, buf, len);
}

int qmc5883l_reg_write(struct compass_dev *compass, uint8_t reg,
		       uint8_t value)
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d)
		return -EINVAL;
	return qmc5883l_write_reg(d, reg, value);
}

/* ------------------------------------------------------------ config ----- */

static int qmc5883l_write_ctrl1(struct qmc5883l *d, int mode)
{
	uint8_t v = (uint8_t)((mode << QMC5883L_MODE_SHIFT) |
			      (d->odr << QMC5883L_ODR_SHIFT) |
			      (d->range << QMC5883L_RNG_SHIFT) |
			      (d->osr << QMC5883L_OSR_SHIFT));

	return qmc5883l_write_reg(d, QMC5883L_CTRL1, v);
}

int qmc5883l_set_range(struct compass_dev *compass, enum qmc5883l_range range)
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d || qmc5883l_lsb_per_ut((int)range) <= 0.0f)
		return -EINVAL;

	d->range = (int)range;
	return qmc5883l_write_ctrl1(d, QMC5883L_MODE_CONT);
}

int qmc5883l_standby(struct compass_dev *compass, bool standby)
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d)
		return -EINVAL;
	return qmc5883l_write_ctrl1(d, standby ? QMC5883L_MODE_STANDBY :
						 QMC5883L_MODE_CONT);
}

/* -------------------------------------------------------------- read ----- */

static int qmc5883l_read_counts(struct qmc5883l *d, int16_t field[3])
{
	uint8_t b[QMC5883L_DATA_LEN];
	int ret;

	ret = qmc5883l_read_regs(d, QMC5883L_DATA_X_LSB, b, sizeof(b));
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		field[i] = qmc5883l_sample(&b[i * 2]);
	return 0;
}

/* compass_ops.read: one burst of six bytes, then STATUS for the overflow. */
static int qmc5883l_read(struct compass_dev *compass, struct compass_sample *s)
{
	struct qmc5883l *d = compass->priv;
	uint8_t status = 0;
	int16_t raw[3];
	int ret;

	ret = qmc5883l_read_counts(d, raw);
	if (ret)
		return ret;
	ret = qmc5883l_read_regs(d, QMC5883L_STATUS, &status, 1);
	if (ret)
		return ret;

	/* A clipped reading looks perfectly normal; refuse it. */
	if (status & QMC5883L_STATUS_OVL)
		return -ERANGE;

	for (int i = 0; i < 3; i++)
		s->field[i] = qmc5883l_ut(raw[i], d->range);
	return 0;
}

static const struct compass_ops qmc5883l_ops = {
	.read = qmc5883l_read,
};

/* ----------------------------------------------------------- extras ------ */

int qmc5883l_read_raw(struct compass_dev *compass, int16_t field[3])
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d || !field)
		return -EINVAL;
	return qmc5883l_read_counts(d, field);
}

int qmc5883l_data_ready(struct compass_dev *compass, bool *ready)
{
	struct qmc5883l *d = to_qmc5883l(compass);
	uint8_t s = 0;
	int ret;

	if (!d || !ready)
		return -EINVAL;

	ret = qmc5883l_read_regs(d, QMC5883L_STATUS, &s, 1);
	if (ret)
		return ret;
	*ready = (s & QMC5883L_STATUS_DRDY) != 0;
	return 0;
}

int qmc5883l_temperature(struct compass_dev *compass, float *relative_c)
{
	struct qmc5883l *d = to_qmc5883l(compass);
	uint8_t b[2];
	int ret;

	if (!d || !relative_c)
		return -EINVAL;

	ret = qmc5883l_read_regs(d, QMC5883L_TEMP_LSB, b, sizeof(b));
	if (ret)
		return ret;
	*relative_c = qmc5883l_temp_c(qmc5883l_sample(b));
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

/*
 * The identity register reads 0xFF, and so does an I2C bus with nothing on
 * it and a pull-up. To tell the two apart, write a register and read it back:
 * a bus that is merely floating cannot store anything.
 */
static int qmc5883l_prove_present(struct compass_dev *compass,
				  struct qmc5883l *d)
{
	uint8_t id = 0, back = 0;
	int ret;

	ret = qmc5883l_read_regs(d, QMC5883L_CHIP_ID, &id, 1);
	if (ret)
		return ret;
	if (id != QMC5883L_CHIP_ID_VALUE) {
		compass_err(compass, "identity register reads 0x%02x, expected "
			    "0x%02x -- an HMC5883L would be at 0x1E, a QMC5883P "
			    "at 0x2C", id, QMC5883L_CHIP_ID_VALUE);
		return -ENODEV;
	}

	ret = qmc5883l_write_reg(d, QMC5883L_SET_RESET, QMC5883L_SET_RESET_VAL);
	if (ret)
		return ret;
	ret = qmc5883l_read_regs(d, QMC5883L_SET_RESET, &back, 1);
	if (ret)
		return ret;
	if (back != QMC5883L_SET_RESET_VAL) {
		compass_err(compass, "nothing is storing writes at 0x%02x (wrote "
			    "0x%02x, read 0x%02x); the 0xFF identity was a "
			    "floating bus", QMC5883L_ADDR,
			    QMC5883L_SET_RESET_VAL, back);
		return -ENODEV;
	}
	return 0;
}

int qmc5883l_probe(struct compass_dev *compass, compass_i2c_bus bus,
		   const struct qmc5883l_config *cfg)
{
	static const struct qmc5883l_config defaults;	/* all zero: defaults */
	struct qmc5883l *d;
	int ret;

	if (!compass || !bus)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;
	if (!cfg)
		cfg = &defaults;

	compass->name = "qmc5883l";

	if (qmc5883l_lsb_per_ut((int)cfg->range) <= 0.0f ||
	    qmc5883l_odr_hz((int)cfg->odr) <= 0.0f ||
	    qmc5883l_osr_ratio((int)cfg->osr) == 0)
		return -EINVAL;

	/*
	 * Anything there at all? See compass_i2c_probe() for why not the first
	 * read. -ENXIO means "not on this bus" and stays at debug level, so
	 * examples/gy271 can try the other chip without a line of noise.
	 */
	ret = compass_i2c_probe(bus, QMC5883L_ADDR, I2C_TIMEOUT_MS);
	if (ret == -ENXIO) {
		compass_dbg(compass, "nothing answered at 0x%02x (%d)",
			    QMC5883L_ADDR, ret);
		return ret;
	}
	if (ret) {
		compass_err(compass, "presence check at 0x%02x failed (%d)",
			    QMC5883L_ADDR, ret);
		return ret;
	}

	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;
	d->range = (int)cfg->range;
	d->odr = (int)cfg->odr;
	d->osr = (int)cfg->osr;

	ret = compass_i2c_attach(&d->bus, bus, QMC5883L_ADDR,
				 compass_config_scl_hz(cfg), I2C_TIMEOUT_MS);
	if (ret) {
		compass_err(compass, "cannot add 0x%02x to the bus (%d)",
			    QMC5883L_ADDR, ret);
		free(d);
		return ret;
	}

	compass->ops = &qmc5883l_ops;
	compass->priv = d;

	ret = qmc5883l_write_reg(d, QMC5883L_CTRL2, QMC5883L_SOFT_RST);
	if (ret)
		goto fail;
	compass_sleep_ms(QMC5883L_RESET_WAIT_MS);

	ret = qmc5883l_prove_present(compass, d);
	if (ret)
		goto fail;

	/* INT_ENB is active low: setting it turns the interrupt pin off, which
	 * is what you want when the pin is not wired anywhere. */
	ret = qmc5883l_write_reg(d, QMC5883L_CTRL2, QMC5883L_INT_ENB);
	if (!ret)
		ret = qmc5883l_write_ctrl1(d, QMC5883L_MODE_CONT);
	if (ret)
		goto fail;

	compass_info(compass, "at 0x%02x: +/-%.0f uT, %.0f Hz, %dx oversampling",
		     QMC5883L_ADDR, (double)qmc5883l_fs_ut(d->range),
		     (double)qmc5883l_odr_hz(d->odr),
		     qmc5883l_osr_ratio(d->osr));
	return 0;

fail:
	compass->ops = NULL;
	compass->priv = NULL;
	compass_bus_detach(&d->bus);
	free(d);
	return ret;
}

void qmc5883l_remove(struct compass_dev *compass)
{
	struct qmc5883l *d = to_qmc5883l(compass);

	if (!d)
		return;

	qmc5883l_write_ctrl1(d, QMC5883L_MODE_STANDBY);
	compass_bus_detach(&d->bus);
	free(d);
	compass->ops = NULL;
	compass->priv = NULL;
}

#endif /* CONFIG_COMPASS_QMC5883L */
