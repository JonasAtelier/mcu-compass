#include <errno.h>
#include <stdlib.h>

#include "compass_priv.h"

#if CONFIG_COMPASS_LIS3MDL

#include "compass_log.h"
#include "lis3mdl.h"
#include "lis3mdl_math.h"
#include "lis3mdl_regs.h"

#define BUS_TIMEOUT_MS		100

/* SPI: 5 MHz unless asked, at most Table 6's 10 MHz. */
#define DEFAULT_SPI_HZ		5000000
#define MAX_SPI_HZ		10000000
/* §5.2: SPC idles high, data is captured on the rising edge -- mode 3. */
#define SPI_MODE		3

struct lis3mdl {
	struct compass_bus bus;
	int range;		/* register codes, so the maths header indexes */
	int odr;		/* the DO code, not the enum */
	int performance;
};

static const struct compass_ops lis3mdl_ops;

/* Our state, or NULL if this compass_dev was not probed by this driver. */
static struct lis3mdl *to_lis3mdl(struct compass_dev *compass)
{
	if (!compass || compass->ops != &lis3mdl_ops)
		return NULL;
	return compass->priv;
}

/* ------------------------------------------------------------ the bus ---- */

/* The register address as this bus wants it; see lis3mdl_regs.h. */
static uint8_t lis3mdl_cmd(const struct lis3mdl *d, uint8_t reg, bool read,
			   size_t len)
{
	if (d->bus.ops->kind == COMPASS_BUS_SPI)
		return (uint8_t)((read ? LIS3MDL_SPI_READ : 0) |
				 (len > 1 ? LIS3MDL_SPI_AUTO_INCREMENT : 0) |
				 (reg & LIS3MDL_SPI_ADDRESS_MASK));
	return (uint8_t)(reg | (len > 1 ? LIS3MDL_I2C_AUTO_INCREMENT : 0));
}

static int lis3mdl_read_regs(struct lis3mdl *d, uint8_t reg, void *buf,
			     size_t len)
{
	return d->bus.ops->transfer(&d->bus, lis3mdl_cmd(d, reg, true, len),
				    NULL, 0, buf, len);
}

static int lis3mdl_write_reg(struct lis3mdl *d, uint8_t reg, uint8_t value)
{
	return d->bus.ops->transfer(&d->bus, lis3mdl_cmd(d, reg, false, 1),
				    &value, 1, NULL, 0);
}

int lis3mdl_reg_read(struct compass_dev *compass, uint8_t reg, void *buf,
		     size_t len)
{
	struct lis3mdl *d = to_lis3mdl(compass);

	if (!d || !buf || len == 0 || reg > LIS3MDL_SPI_ADDRESS_MASK)
		return -EINVAL;
	return lis3mdl_read_regs(d, reg, buf, len);
}

int lis3mdl_reg_write(struct compass_dev *compass, uint8_t reg,
		      uint8_t value)
{
	struct lis3mdl *d = to_lis3mdl(compass);

	if (!d || reg > LIS3MDL_SPI_ADDRESS_MASK)
		return -EINVAL;
	return lis3mdl_write_reg(d, reg, value);
}

/* ------------------------------------------------------------ config ----- */

/*
 * Every control register, from scratch, CTRL_REG3 last so conversion starts
 * on the settings just written. The reserved bits are written 0, as Tables
 * 23, 26, 29 and 32 require. The temperature sensor is always on: Table 4
 * acquires it with each conversion.
 */
static int lis3mdl_configure(struct lis3mdl *d)
{
	const uint8_t ctrl[][2] = {
		{ LIS3MDL_CTRL_REG1,
		  (uint8_t)(LIS3MDL_TEMP_EN |
			    (d->performance << LIS3MDL_OM_SHIFT) |
			    (d->odr << LIS3MDL_DO_SHIFT)) },
		{ LIS3MDL_CTRL_REG2, (uint8_t)(d->range << LIS3MDL_FS_SHIFT) },
		{ LIS3MDL_CTRL_REG4,
		  (uint8_t)(d->performance << LIS3MDL_OMZ_SHIFT) },
		{ LIS3MDL_CTRL_REG5, LIS3MDL_BDU },
		{ LIS3MDL_CTRL_REG3, LIS3MDL_MD_CONTINUOUS },
	};
	int ret;

	for (size_t i = 0; i < sizeof(ctrl) / sizeof(ctrl[0]); i++) {
		ret = lis3mdl_write_reg(d, ctrl[i][0], ctrl[i][1]);
		if (ret)
			return ret;
	}
	return 0;
}

int lis3mdl_set_range(struct compass_dev *compass, enum lis3mdl_range range)
{
	struct lis3mdl *d = to_lis3mdl(compass);
	int ret;

	if (!d || lis3mdl_fs_gauss((int)range) == 0)
		return -EINVAL;

	ret = lis3mdl_write_reg(d, LIS3MDL_CTRL_REG2,
				(uint8_t)((int)range << LIS3MDL_FS_SHIFT));
	if (!ret)
		d->range = (int)range;
	return ret;
}

int lis3mdl_standby(struct compass_dev *compass, bool standby)
{
	struct lis3mdl *d = to_lis3mdl(compass);

	if (!d)
		return -EINVAL;
	return lis3mdl_write_reg(d, LIS3MDL_CTRL_REG3,
				 standby ? LIS3MDL_MD_POWER_DOWN :
					   LIS3MDL_MD_CONTINUOUS);
}

/* -------------------------------------------------------------- read ----- */

static int lis3mdl_read_counts(struct lis3mdl *d, int16_t field[3])
{
	uint8_t b[LIS3MDL_DATA_LEN];
	int ret;

	ret = lis3mdl_read_regs(d, LIS3MDL_OUT_X_L, b, sizeof(b));
	if (ret)
		return ret;
	for (int i = 0; i < 3; i++)
		field[i] = lis3mdl_sample(&b[i * 2]);
	return 0;
}

/* compass_ops.read: one six-byte burst; past full scale is -ERANGE. */
static int lis3mdl_read(struct compass_dev *compass, struct compass_sample *s)
{
	struct lis3mdl *d = compass->priv;
	int16_t raw[3];
	int ret;

	ret = lis3mdl_read_counts(d, raw);
	if (ret)
		return ret;
	if (lis3mdl_clipped(raw, d->range))
		return -ERANGE;
	for (int i = 0; i < 3; i++)
		s->field[i] = lis3mdl_ut(raw[i], d->range);
	return 0;
}

static const struct compass_ops lis3mdl_ops = {
	.read = lis3mdl_read,
};

/* ----------------------------------------------------------- extras ------ */

int lis3mdl_read_raw(struct compass_dev *compass, int16_t field[3])
{
	struct lis3mdl *d = to_lis3mdl(compass);

	if (!d || !field)
		return -EINVAL;
	return lis3mdl_read_counts(d, field);
}

int lis3mdl_data_ready(struct compass_dev *compass, bool *ready)
{
	struct lis3mdl *d = to_lis3mdl(compass);
	uint8_t status = 0;
	int ret;

	if (!d || !ready)
		return -EINVAL;

	ret = lis3mdl_read_regs(d, LIS3MDL_STATUS_REG, &status, 1);
	if (ret)
		return ret;
	*ready = (status & LIS3MDL_ZYXDA) != 0;
	return 0;
}

int lis3mdl_temperature(struct compass_dev *compass, float *relative_c)
{
	struct lis3mdl *d = to_lis3mdl(compass);
	uint8_t b[2];
	int ret;

	if (!d || !relative_c)
		return -EINVAL;

	ret = lis3mdl_read_regs(d, LIS3MDL_TEMP_OUT_L, b, sizeof(b));
	if (ret)
		return ret;
	*relative_c = lis3mdl_temp_c(lis3mdl_sample(b));
	return 0;
}

/* ------------------------------------------------------- probe/remove ---- */

static const char *const performance_name[] = {
	"low-power", "medium", "high", "ultra-high",
};

/* What both probes refuse before touching the bus. */
static int lis3mdl_check_config(const struct lis3mdl_config *cfg)
{
	if (lis3mdl_fs_gauss((int)cfg->range) == 0 ||
	    lis3mdl_do_code((int)cfg->odr) < 0 ||
	    (unsigned int)cfg->performance > LIS3MDL_FIELD_CODE_MAX)
		return -EINVAL;
	return 0;
}

static struct lis3mdl *lis3mdl_alloc(const struct lis3mdl_config *cfg)
{
	struct lis3mdl *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->range = (int)cfg->range;
	d->odr = lis3mdl_do_code((int)cfg->odr);
	d->performance = (int)cfg->performance;
	return d;
}

/*
 * The chip half of both probes, with d's bus attached: identify, configure,
 * bind. On failure the bus is detached and d freed. address is for the log;
 * 0 on SPI.
 */
static int lis3mdl_start(struct compass_dev *compass, struct lis3mdl *d,
			 uint8_t address)
{
	uint8_t id = 0;
	int ret;

	compass->ops = &lis3mdl_ops;
	compass->priv = d;

	ret = lis3mdl_read_regs(d, LIS3MDL_WHO_AM_I, &id, 1);
	if (ret)
		goto fail;
	if (id != LIS3MDL_WHO_AM_I_VALUE) {
		if (address)
			compass_err(compass, "WHO_AM_I at 0x%02x is 0x%02x, "
				    "not 0x%02x", address, id,
				    LIS3MDL_WHO_AM_I_VALUE);
		else
			compass_err(compass, "WHO_AM_I on SPI is 0x%02x, not "
				    "0x%02x -- 0x00 or 0xff is nothing there",
				    id, LIS3MDL_WHO_AM_I_VALUE);
		ret = -ENODEV;
		goto fail;
	}

	ret = lis3mdl_configure(d);
	if (ret)
		goto fail;

	if (address)
		compass_info(compass, "at 0x%02x: +/-%d gauss, %.3g Hz, %s",
			     address, lis3mdl_fs_gauss(d->range),
			     (double)lis3mdl_odr_hz(d->odr),
			     performance_name[d->performance]);
	else
		compass_info(compass, "on SPI: +/-%d gauss, %.3g Hz, %s",
			     lis3mdl_fs_gauss(d->range),
			     (double)lis3mdl_odr_hz(d->odr),
			     performance_name[d->performance]);
	return 0;

fail:
	compass->ops = NULL;
	compass->priv = NULL;
	compass_bus_detach(&d->bus);
	free(d);
	return ret;
}

int lis3mdl_probe_i2c(struct compass_dev *compass, compass_i2c_bus bus,
		      const struct lis3mdl_config *cfg)
{
	static const struct lis3mdl_config defaults;	/* all zero: defaults */
	struct lis3mdl *d;
	uint8_t address;
	int ret;

	if (!compass || !bus)
		return -EINVAL;
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;
	if (!cfg)
		cfg = &defaults;

	compass->name = "lis3mdl";

	address = cfg->address ? cfg->address : LIS3MDL_ADDR_HIGH;
	if ((address != LIS3MDL_ADDR_LOW && address != LIS3MDL_ADDR_HIGH) ||
	    lis3mdl_check_config(cfg))
		return -EINVAL;

	/*
	 * Anything there at all? See compass_i2c_probe() for why not the first
	 * read. -ENXIO means "not on this bus" and stays at debug level, so a
	 * caller can try the other address without a line of noise.
	 */
	ret = compass_i2c_probe(bus, address, BUS_TIMEOUT_MS);
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

	d = lis3mdl_alloc(cfg);
	if (!d)
		return -ENOMEM;
	ret = compass_i2c_attach(&d->bus, bus, address,
				 compass_config_scl_hz(cfg), BUS_TIMEOUT_MS);
	if (ret) {
		compass_err(compass, "cannot add 0x%02x to the bus (%d)",
			    address, ret);
		free(d);
		return ret;
	}
	return lis3mdl_start(compass, d, address);
}

int lis3mdl_probe_spi(struct compass_dev *compass,
		      const struct lis3mdl_spi_config *spi,
		      const struct lis3mdl_config *cfg)
{
	static const struct lis3mdl_config defaults;	/* all zero: defaults */
	struct lis3mdl *d;
	int ret;
#ifdef ESP_PLATFORM
	uint32_t clock_hz;
#endif

	if (!compass || !spi)
		return -EINVAL;
#ifndef ESP_PLATFORM
	if (!spi->bus || !spi->cs_port)
		return -EINVAL;
#endif
	/* Bound already: a second probe would leak the first binding. */
	if (compass->ops || compass->priv)
		return -EBUSY;
	if (!cfg)
		cfg = &defaults;

	compass->name = "lis3mdl";

	if (lis3mdl_check_config(cfg))
		return -EINVAL;
#ifdef ESP_PLATFORM
	clock_hz = spi->clock_hz ? spi->clock_hz : DEFAULT_SPI_HZ;
	if (clock_hz > MAX_SPI_HZ)
		return -EINVAL;
#endif

	d = lis3mdl_alloc(cfg);
	if (!d)
		return -ENOMEM;
#ifdef ESP_PLATFORM
	ret = compass_spi_attach(&d->bus, spi->host, spi->cs_pin, clock_hz,
				 SPI_MODE, BUS_TIMEOUT_MS);
#else
	ret = compass_spi_attach(&d->bus, spi->bus, spi->cs_port, spi->cs_pin,
				 BUS_TIMEOUT_MS);
#endif
	if (ret) {
		compass_err(compass, "cannot add the SPI device (%d)", ret);
		free(d);
		return ret;
	}
	return lis3mdl_start(compass, d, 0);
}

void lis3mdl_remove(struct compass_dev *compass)
{
	struct lis3mdl *d = to_lis3mdl(compass);

	if (!d)
		return;

	lis3mdl_write_reg(d, LIS3MDL_CTRL_REG3, LIS3MDL_MD_POWER_DOWN);
	compass_bus_detach(&d->bus);
	free(d);
	compass->ops = NULL;
	compass->priv = NULL;
}

#endif /* CONFIG_COMPASS_LIS3MDL */
