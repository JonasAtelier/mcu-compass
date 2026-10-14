# API usage and heading

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Device lifecycle

Initialise the bus, probe once, then poll for data ready before reading.
The [README quick start](../README.md#quick-start) shows a complete STM32
helper with a bounded wait and cleanup. The [ESP-IDF example](../examples/esp/qmc5883p/main/main.c)
shows the corresponding bus setup and polling loop.

One `compass_log_sample()` line looks like this (values illustrative):

```
compass: qmc5883p: t 1234000 us  x   +21.40  y    -3.10  z   -38.90 uT
```

Tear down with the matching `*_remove(&compass)`; the bus is left alone.

**Bus handles.** QMC5883L, QMC5883P and IST8310 probes take an
`i2c_master_bus_handle_t` on ESP-IDF or an initialised `I2C_HandleTypeDef *`
on STM32. ESP-IDF adds a device at `cfg->scl_hz` (0 selects 400 kHz) and
removes it on `*_remove()`; STM32 uses the bus handle's configured speed. For both QMC chips,
configure I2C at 400 kHz or less.
QMC addresses are fixed; IST8310 has a configurable address. LIS3MDL offers
separate I2C and SPI probes, and CMPS14 takes a UART handle. See the
[chip reference](chips.md) for their configurations.

**A `struct compass_dev` must start zeroed** — static storage or `= { 0 }`,
as above. Probe on one that is already bound (non-NULL `ops` or `priv`)
returns `-EBUSY` and touches nothing, rather than leak the first binding.
`*_remove()` and probe-failure cleanup clear `ops` and `priv`, allowing
a fresh probe. They preserve the mounting matrix; the chip name may remain
set for logging. A refused probe on an already-bound device leaves it intact.

**A clipped reading is an error, not a sample.** Past full scale both QMC chips
raise an overflow flag, and a clipped vector looks perfectly normal — so
`compass_read()` returns `-ERANGE` and leaves `*s` alone. Widen the range or
move the magnet. `*_read_raw()` still gives you the counts.

**Timestamps.** `s.t_us` comes from `compass_time_us()`, taken just before
the bus transfer. On ESP-IDF that is `esp_timer_get_time()`. On the HAL the
library's default is weak and only
`HAL_GetTick() * 1000`: whole milliseconds, wrapping after 49.7 days.
**Define your own `int64_t compass_time_us(void)` for real microseconds** and
it replaces the default at link time — the `DWT->CYCCNT` cycle counter
(enable `CoreDebug->DEMCR` TRCENA and `DWT->CTRL` CYCCNTENA, then divide by
the core clock in MHz and extend the 32-bit count to 64; an F7 may also need
`DWT->LAR` unlocked first), or a free-running timer at 1 MHz.

**Delays.** On ESP-IDF, probes and the self-test wait with `vTaskDelay()`,
rounded up and plus one tick, so never shorter than asked; at the default
100 Hz tick even a 2 ms wait takes 10–20 ms. On the HAL they wait with
`HAL_Delay()`, which already
adds one tick to what it is asked, so no wait is ever shorter than the chip
needs. Waits busy-wait on SysTick: probe and self-test from thread context,
never from an interrupt at or above SysTick's priority. A QMC5883L probe
waits 10–11 ms, a QMC5883P probe 1–2 ms, an IST8310 probe 50–51 ms;
`compass_read()` does not wait for a new conversion; bus transfers are
blocking, including the CMPS14 UART command and reply.
If your app replaces the weak `HAL_Delay()` (an RTOS often does), the "never
short" guarantee is that replacement's.

**Logging.** On ESP-IDF everything goes under the log tag `compass`,
prefixed with the chip name, and `compass_dbg()` is `ESP_LOGD`. On the HAL
every line goes through `printf()` as `compass: <chip>: ...`.
Your own code can use the same `compass_err()`, `compass_warn()`,
`compass_info()` and `compass_dbg()` from `compass_log.h`. `compass_dbg()` is
compiled out unless `CONFIG_COMPASS_DEBUG` is 1 (stm-forge:
`COMPASS_DEBUG = y`). There are no levels beyond that: printf() is the one
sink.

**Chip extras** take the same `struct compass_dev *` and return `-EINVAL` if
another driver probed it: raw counts, data ready, range, standby and raw
registers on both QMC parts; relative die temperature on the L; the
datasheet self-test on the P. The IST8310 has raw counts, data ready and raw
registers.

### Mount: get the axes into your board's frame

`compass_dev.mount` is a 3×3 matrix of -1/0/+1, `board = mount × chip`,
exactly as in stm-imu. All zero means identity. `compass_heading_deg()` wants
the board frame **X right, Y forward, Z up**, and reports where +Y points. So
if your chip sits rotated 90° about Z, so that its X axis points forward:

```c
static const int8_t chip_x_forward[3][3] = {
	{ 0, -1, 0 },   /* board X = -chip Y */
	{ 1,  0, 0 },   /* board Y =  chip X */
	{ 0,  0, 1 },
};
memcpy(compass.mount, chip_x_forward, sizeof(chip_x_forward));
```

The QMC5883P's positive axes are drawn in the datasheet's Figure 2: seen from
above with pin 1 top left, X points left, Y points down the page (towards
you), Z up out of the board.

### Calibration and heading

The steel, magnets and DC current around the sensor shift the field by far
more than the earth's own 25–65 µT, and that offset is fixed to your board,
not to the world. **Hard-iron offset**, the two-minute version:

1. Log `field[0]` and `field[1]` while turning the board slowly through a full
   circle, flat.
2. Take the midpoint of each: `off_x = (max_x + min_x) / 2`, same for Y.
3. Subtract those from every reading before `compass_heading_deg()`.

That is what `HARD_IRON_*` in the examples is for. Soft-iron distortion — the
circle coming out as an ellipse — needs a scale per axis, or a full 3×3
correction if the ellipse is tilted; that is beyond what a driver should
decide for you.

- **The heading assumes the board is flat.** Tilt it and X and Y pick up the
  vertical field, which in temperate latitudes is most of the field. Tilt
  compensation needs an accelerometer — pair it with
  [`mcu-imu`](https://github.com/JonasAtelier/mcu-imu).
- **Declination** turns magnetic north into true north. It depends on where
  you are and drifts year to year; look it up.

## Errors

| errno | from | means |
|---|---|---|
| `-EINVAL` | any | NULL argument; an enum out of range; on the HAL, a `reg_read` longer than 65535 bytes; a `compass_dev` another driver probed; on ESP-IDF, `ESP_ERR_INVALID_ARG` |
| `-EBUSY` | probe | that `compass_dev` is already bound. Nothing was touched |
| `-ENXIO` | probe | nothing ACKed at the chip's address — or, for the CMPS14, nothing answered on the UART within 100 ms — checked with `i2c_master_probe()` or `HAL_I2C_IsDeviceReady()` before anything is allocated or written. On a GY-271, try the other chip |
| `-ENODEV` | probe | something answered but it is not the chip: QMC5883P `CHIPID` not 0x80; QMC5883L identity not 0xFF, or a write that did not read back (a floating bus); IST8310 `WAI` not 0x10; LIS3MDL `WHO_AM_I` not 0x3D — on SPI, which has no acknowledge, also an empty bus |
| `-EAGAIN` | any | HAL only: the HAL answered `HAL_BUSY`: the bus or the handle is in use, or the handle was never `HAL_I2C_Init()`ed. On the F4, also a bus with no pull-ups or SDA stuck low during the probe's presence check — see below |
| `-ERANGE` | read | the chip flagged overflow: the reading is clipped. On the IST8310 and the LIS3MDL, an axis past the datasheet's measurement range |
| `-ENODATA` | read | IST8310 only: the measurement the last read started has not landed yet. Read again later; nothing is wrong |
| `-ETIMEDOUT` | any | a bus transfer timed out (`ESP_ERR_TIMEOUT`, `HAL_TIMEOUT`), or the QMC5883P self-test saw no data ready within 100 ms |
| `-EIO` | any | any other bus failure (`HAL_ERROR`; on ESP-IDF `ESP_ERR_INVALID_STATE` and the rest), including a NACK in the middle of a transfer |
| `-EMSGSIZE` | any | ESP-IDF only: `ESP_ERR_INVALID_SIZE` from the bus |
| `-ENOMEM` | probe | out of heap, or on ESP-IDF the bus could not add the device |

A chip that NACKs its address is `HAL_ERROR` from `HAL_I2C_IsDeviceReady()`,
`-ENXIO`, on the F4 and the F7. On the F4, a bus with no pull-ups or SDA stuck
low sets the peripheral's BUSY flag, so the probe gives up after 25 ms with
`-EAGAIN`; `-ETIMEDOUT` from the presence check means a START never
completed. Check the wiring for either; unlike `-ENXIO`, both are logged.
`-EAGAIN` is never `-EBUSY`, which a probe keeps for "already bound".

`strerror(-ret)` names any of them. **Do not use a sample when
`compass_read()` returns an error**: it writes `*s` only on success, so the
old values are still there.
