<p align="center">
  <img src="assets/compass.svg"
       alt="A compass turning in the earth's field, and the two axes it produces"
       width="100%">
</p>

# compass

<p align="center">
  <img alt="platform: ESP-IDF | STM32 HAL" src="https://img.shields.io/badge/platform-ESP--IDF%20%7C%20STM32%20HAL-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="language: C" src="https://img.shields.io/badge/language-C-70e1b2?style=flat-square&labelColor=0d2238">
  <img alt="license: MIT" src="https://img.shields.io/badge/license-MIT-f2bd68?style=flat-square&labelColor=0d2238">
</p>

A small Linux-IIO-style magnetometer subsystem for ESP-IDF and the STM32 HAL
(F4 and F7),
plus five chip drivers: the QST **QMC5883L** and the QST **QMC5883P** — the
two parts a "GY-271" compass board actually carries — and the iSentek
**IST8310**, the compass on many drone GPS masts, all over I2C; the ST
**LIS3MDL**, over I2C or SPI; and the Robot Electronics **CMPS14** module,
over its serial link. Every
chip's probe fills one `struct compass_dev`, and from then on
`compass_read()` gives you the same `struct compass_sample` whatever the chip
is: the field in **microtesla**, rotated into your board's frame, with a
timestamp from `compass_time_us()`. `compass_heading_deg()` turns that into
degrees from north. Every call returns 0 or a negative errno. Chip-specific
settings (ranges, filters, self-test) stay chip-specific.

**One library, two platforms.** It replaces esp-compass and stm-compass. The
chip drivers in `src/` are the same C on both; only the bus, the clock and
the sleep under them differ, in `src/compass_port_esp.c` (ESP-IDF
`i2c_master`, `esp_timer`, FreeRTOS) and `src/compass_port_stm.c` (HAL). Each
port is empty on the other platform, chosen by `ESP_PLATFORM`, which ESP-IDF
defines for every component. Probes take the platform's own bus handle:
`i2c_master_bus_handle_t` on ESP-IDF, `I2C_HandleTypeDef *` on the HAL. Only
the ESP-IDF configs have `scl_hz`, because only there does the driver add its
own device to the bus; on the HAL the bus speed is the handle's.

**One bus layer, three bus kinds.** Under the chips sits a regmap-style ops
table, `struct compass_bus_ops` in `src/compass_priv.h`: each chip holds one
`struct compass_bus` and talks to it only through `transfer(cmd, tx, rx)`.
The kind — I2C, SPI or UART — is picked at run time by which attach the
probe calls; the platform stays a link-time choice. `transfer` is a command
with a reply, not a register read/write, because a serial-command module
(command byte, optional arguments, a fixed-length answer) does not fit
register access; on I2C and SPI the command is the register address, and
`compass_reg_read()`/`compass_reg_write()` are the usual shape on top. The
CMPS14 is the UART user: each of its serial commands is one `transfer`.

> **Hardware unverified.** The host tests in `test/` pass. Nothing here has
> run on a physical chip.

## Install

**With [esp-forge](https://github.com/JonasAtelier/esp-forge) or
[stm-forge](https://github.com/JonasAtelier/stm-forge).** The catalog line in
`profiles/pkgversion` is the same for both:

```make
compass_GIT=https://github.com/JonasAtelier/compass@stable
```

and a target turns the library and its chips on in `configs/<target>_defconfig`:

```make
PKG_compass         = y
COMPASS_QMC5883L    = y
COMPASS_QMC5883P    = y
COMPASS_IST8310     = y
COMPASS_LIS3MDL     = y
COMPASS_CMPS14      = y
# COMPASS_DEBUG is not set
```

or with `make menu`. esp-forge turns each line into `CONFIG_COMPASS_<CHIP>=y`
in `sdkconfig.defaults`; stm-forge passes it as `-DCONFIG_COMPASS_<CHIP>=1`.
`COMPASS_DEBUG = y` turns on `compass_dbg()` lines on the HAL; on ESP-IDF the
log level decides.

**Plain ESP-IDF.** Clone into your project's `components/` directory. A local
component is named after its directory, and that name is what you
`REQUIRES`:

```sh
git clone https://github.com/JonasAtelier/compass components/compass
```

Your `main` component sees it without any `REQUIRES`, as the examples in
`examples/esp/` do. Any other component needs
`idf_component_register(SRCS "foo.c" REQUIRES compass driver)`. Enable the
chips under `idf.py menuconfig` → **Component config → Compass (magnetometer)
subsystem**, or put `CONFIG_COMPASS_<CHIP>=y` in your `sdkconfig.defaults`.
With the subsystem on and no chip picked, the build warns. ESP-IDF 5.2 or
newer is required for the `i2c_master` API.

**Anywhere else on STM32.** Copy `include/` and `src/` into an STM32 project, compile
`src/*.c`, add `include/` to the include path, and define what the forge
would: `CMSIS_device_header` as your family header (`"stm32f4xx.h"` or
`"stm32f7xx.h"`), `USE_HAL_DRIVER`, and `CONFIG_COMPASS_<CHIP>=1` for each
chip you want.

**No chip driver is built by default.** Like a kernel subsystem, the core is
always built and each driver is opt-in: a chip whose option is absent or 0
compiles to nothing. With the library on and no chip picked, the ESP-IDF
CMakeLists warns and `src/compass_port_stm.c` emits a `#warning`.

The rest of this section is the STM32 HAL's.

**The HAL modules are yours to enable.** The I2C drivers call the HAL's I2C,
the LIS3MDL also its SPI, and the CMPS14 its UART, so your app's `stm32f4xx_hal_conf.h` (or
`stm32f7xx_hal_conf.h`) must define them, and their `.c` must be in the
build:

| Option | Needs in hal_conf | HAL source |
|---|---|---|
| `COMPASS_QMC5883L`, `COMPASS_QMC5883P`, `COMPASS_IST8310` | `HAL_I2C_MODULE_ENABLED` | `stm32f{4,7}xx_hal_i2c.c` |
| `COMPASS_LIS3MDL` | `HAL_I2C_MODULE_ENABLED` and `HAL_SPI_MODULE_ENABLED` (plus GPIO, for chip select) | `stm32f{4,7}xx_hal_i2c.c`, `_hal_spi.c` |
| `COMPASS_CMPS14` | `HAL_UART_MODULE_ENABLED` | `stm32f{4,7}xx_hal_uart.c` |

stm-forge's `mcu/stm32f4.cmake` and `mcu/stm32f7.cmake` already list it,
plus `_hal_i2c_ex.c` (for the `HAL_I2CEx_ConfigAnalogFilter()` that
CubeMX-style init calls); each compiles to nothing until hal_conf enables the
module. A chip whose module is off stops the build with an `#error` that says
which define is missing.

**printf() is the log.** Lines go out through `printf()`, so retarget
newlib's `_write()` to a UART. stm-forge links newlib-nano
(`--specs=nano.specs`), whose `printf()` drops `%f` unless the app links with
`-u _printf_float`: add `target_link_options(firmware PRIVATE -u _printf_float)`
to your app's `CMakeLists.txt`, or `compass_log_sample()` prints blanks where
the numbers go.

**`compass_heading_deg()` needs libm.** It calls `atan2f()` and `fmodf()`.
stm-forge links the maths library for every firmware, so there is nothing to
do there; any other build that calls it needs `-lm`, or the link stops at
`undefined reference to 'atan2f'`.

## Which chip is on my GY-271?

Boards are still sold as "GY-271 HMC5883L", and have carried three different
chips over the years, each with its own register map:

| | QMC5883P | QMC5883L | HMC5883L |
|---|---|---|---|
| Maker | QST | QST | Honeywell (discontinued) |
| I2C address | **0x2C** | **0x0D** | 0x1E |
| Identity | `CHIPID` 0x00 reads `0x80` | 0x0D reads `0xFF` | 0x0A–0x0C read `H43` |
| Seen on | most boards made now | most boards of recent years | old stock |
| Here | `qmc5883p_probe()` | `qmc5883l_probe()` | not supported |

The address is fixed by the chip, so the one that answers names it.
**The gy271 example does exactly that**: it tries the P, then the L, then tells
you whether an HMC5883L is sitting at 0x1E. Both probes return `-ENXIO` —
and log nothing unless `COMPASS_DEBUG` is on — when nothing is at their
address, so you can chain them the same way:

```c
int ret = qmc5883p_probe(&compass, &hi2c1, NULL);
if (ret == -ENXIO)
	ret = qmc5883l_probe(&compass, &hi2c1, NULL);
```

Build both drivers for that (`COMPASS_QMC5883L = y` and
`COMPASS_QMC5883P = y`).

## Pins

Pins below are the ones the STM32 examples use, on an F4; the ESP-IDF
examples set theirs at the top of `main.c`. On STM32: **I2C1 on PB8 (SCL) and PB9
(SDA), AF4**. Any pair your chip routes to an I2C peripheral works; check the
alternate-function table in its datasheet. The bus init — pins, clocks,
speed — is the app's; each example shows one. Neither driver needs an
interrupt or data-ready line.

### GY-271 — `examples/*/gy271`, `examples/*/qmc5883p`, `examples/*/qmc5883l`

| pin | goes to | note |
|---|---|---|
| VCC | 3.3 V | most GY-271s carry a regulator and also take 5 V; check yours. The bare chips are 2.5–3.6 V parts |
| GND | GND | shared ground, always |
| SCL | PB8 | I2C1_SCL, AF4, open drain |
| SDA | PB9 | I2C1_SDA, AF4, open drain |
| DRDY | leave open | the QMC5883P **has no DRDY pin** (Table 5), so on a P board this goes nowhere. On an L board it works, but the driver polls `STATUS` over I2C instead |

There is no address strap on either QMC chip: one of each per bus, at most. On a
GY-86 the QMC5883L sits behind the MPU-6050 and is reachable only once
`stm-imu`'s MPU-6050 driver has put the auxiliary bus in bypass.

The QMC5883P bare chip, for a board of your own (datasheet Table 5, Figure 6):
SCK, SDA, VDD, GND, and **C1, a 4.7 µF low-ESR ceramic to ground** (§4.3.3) —
the set/reset reservoir; without it the part cannot renew its offset. Pull-ups
of 2.7 kΩ for a bus under 10 cm (§4.1). Keep ferrous parts and copper traces
away from the package (§4.3). The examples only turn on the STM32's weak
internal pull-ups; a GY-271 carries its own, and a bare chip needs external
ones on SDA and SCL.

### IST8310 module — `examples/stm/ist8310`

| pin | goes to | note |
|---|---|---|
| VCC | 3.3 V | the bare chip is a 1.72–3.6 V part (§4.2); a module may carry a regulator |
| GND | GND | shared ground, always |
| SCL | PB8 | I2C1_SCL, AF4, open drain |
| SDA | PB9 | I2C1_SDA, AF4, open drain |
| CAD0, CAD1 | strap for the address | both floating is 0x0E (§6.1.1); `address = 0` means that |
| DRDY | leave open | the driver polls `STAT1` over I2C |
| RSTN | leave open | pulled high inside (§2.2); the driver soft-resets instead |

For a board of your own (§2.3, Figure 2): **C1, a 4.7 µF ceramic to ground**,
the set/reset reservoir; 0.1 µF on AVDD and on DVDD; 4.7 kΩ pull-ups on SDA
and SCL (§6.1); VPP floating.

## Use

Initialise the bus, probe, then read:

```c
static struct compass_dev compass;     /* zeroed */
struct compass_sample s;

if (qmc5883p_probe(&compass, &hi2c1, NULL))  /* probe logs why it failed */
	return;

if (!compass_read(&compass, &s))
	compass_log_sample(&compass, &s);
```

One `compass_log_sample()` line looks like this (values illustrative):

```
compass: qmc5883p: t 1234000 us  x   +21.40  y    -3.10  z   -38.90 uT
```

Tear down with the matching `*_remove(&compass)`; the bus is left alone.

**Bus handles.** On ESP-IDF every probe takes an `i2c_master_bus_handle_t`
the app has created, and adds its own device at `cfg->scl_hz` (0 is 400 kHz);
`*_remove()` takes it off again. On the HAL every probe takes an
`I2C_HandleTypeDef *` the app has `HAL_I2C_Init()`ed. Their addresses are fixed, so there is no address in the
config; the driver shifts the 7-bit address for the HAL itself. Bus speed is
the handle's business: 400 kHz or less suits both (the QMC5883P's §5.4 allows
standard and fast mode only).

**A `struct compass_dev` must start zeroed** — static storage or `= { 0 }`,
as above. Probe on one that is already bound (non-NULL `ops` or `priv`)
returns `-EBUSY` and touches nothing, rather than leak the first binding.
`*_remove()` and a failed probe zero it again, so it can be probed afresh.

**A clipped reading is an error, not a sample.** Past full scale both chips
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
`compass_read()` never waits.
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

### Calibrate it or the heading is fiction

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
  [`stm-imu`](https://github.com/JonasAtelier/stm-imu).
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

## Chips

### QMC5883P

QST datasheet QST-PD-B002-22 Rev E, in `docs/` (download it from qstcorp.com
if your clone lacks it; PDFs are not committed). Every register value in
`src/qmc5883p_regs.h` and `src/qmc5883p.c` cites its section or table.

| Range | Full scale | Counts per µT |
|---|---|---|
| `RANGE_3000UT` (default) | ±30 G | 10 |
| `RANGE_1200UT` | ±12 G | 25 |
| `RANGE_800UT` | ±8 G — §7's choice | 37.5 |
| `RANGE_200UT` | ±2 G | 150 |

Full scale is 30000 counts on every range, which is exactly where `OVFL` trips
(§9.2.2). The all-zero config is the chip's own reset values in normal mode;
The qmc5883p examples use §7.1's ±8 G, 200 Hz, OSR1 8, OSR2 8 instead. For a
compass on a clean board, ±2 G gives the finest resolution.

**What probe writes**, checked against §7.1 and §7.2 byte for byte in the
tests: `0BH = 0x80` (soft reset, §7.6), `29H = 0x06`, `0BH` range with
set/reset on, `0AH = 0x00` (suspend), `0AH` mode.

**Register 29H is undocumented.** Every setup sequence in §7 starts by
writing `0x06` to it, "define the sign for X Y and Z axis", and the register
map does not list it. The driver writes it because QST does.

**Suspend between modes** (§9.2.3): normal, single and continuous may not
switch directly. Every mode change in the driver goes through suspend first.

**Normal or continuous.** Normal mode (the default) measures every 1/ODR.
`.continuous = true` runs back to back, up to 1500 Hz at 2.2 mA (Table 2),
and ignores ODR.

**Reading STATUS clears both flags** (§9.2.2). So a `qmc5883p_data_ready()`
poll would swallow an overflow; the driver latches it and the next
`compass_read()` still returns `-ERANGE`.

**Self-test** (`qmc5883p_self_test()`, §7.3): the field without and with the
chip's internal stimulus, as `(x1 − x2, y2 − y1, z2 − z1)` — X is taken the
other way round, exactly as step 8 prints it. **There is no pass/fail**: the
datasheet says to compare against a threshold and never gives one, so you get
the deltas; a live axis moves well off zero. §7.3's `0BH = 0x40` also sets the
±30 G range for the second reading, and §7.3 starts from reset, so the first
is at 30 G there too. The driver therefore switches to ±30 G (and clears any
stale data-ready) before the first reading as well: both readings, and the
deltas, are at 1000 LSB/G whatever range you configured. It restores your
range and mode afterwards, and clears the flags the stimulated reading left —
including any overflow latched before the self-test that no `compass_read()`
had reported yet. Wait for `qmc5883p_data_ready()` before the next read, or
you get the stimulated sample.

Step 3's wait for data ready polls `STATUS` 1 ms or more apart until more
than 100 ms have passed by `HAL_GetTick()`, then gives up with `-ETIMEDOUT`.
§7.3 step 6 allows 5 ms per measurement at this CTRL1; 100 ms is 20x that,
so only a part that never measures waits it out.

**Gaps in the datasheet**, filled with a stated assumption:

- No soft-reset time. The driver waits the power-on time, 250 µs max
  (Table 7), rounded up to `HAL_Delay(1)`: 1–2 ms.
- Burst reads are not described as auto-incrementing in so many words; §8.2.4
  has the master ACK each byte to get the next, which is what the driver
  relies on to read all six data bytes in one `HAL_I2C_Mem_Read()`.

No temperature output, no DRDY pin, no single mode (the driver does not use
it).

### QMC5883L

No datasheet ships here. Every address, bit and constant appears in two
independent drivers that agree: the Linux kernel's
[`drivers/iio/magnetometer/qmc5883l.c`](https://github.com/torvalds/linux/blob/master/drivers/iio/magnetometer/qmc5883l.c)
and [ESPHome's `qmc5883l`](https://github.com/esphome/esphome/tree/dev/esphome/components/qmc5883l).

| Range | Full scale | Counts per µT |
|---|---|---|
| `RANGE_200UT` (default) | ±2 G | 120 |
| `RANGE_800UT` | ±8 G | 30 |

**The identity register cannot prove presence.** It reads `0xFF`, which is
what a bus with a pull-up and nothing attached reads too. So probe also writes
the SET/RESET register and reads it back; a floating bus cannot store
anything. The tests have an explicit empty-bus case.

**Temperature is relative** (`qmc5883l_temperature()`): 100 counts per
degree against an uncalibrated zero. Good for tracking drift, useless as a
thermometer.

The 10 ms wait after soft reset is carried over from esp-qmc5883l-mag; neither
reference driver states one.

### IST8310

iSentek datasheet Version 1.2, in `docs/` (LCSC carries it as part C2683055;
PDFs are not committed). Every register value in `src/ist8310_regs.h` and
`src/ist8310.c` cites its section.

One range: ±1600 µT on X and Y, ±2500 µT on Z, **0.3 µT per count** (§4.4
RESO; the same table's 3.3 LSB/µT is that rounded). Four addresses, 0x0C–0x0F,
by the CAD pins; `address = 0` is 0x0E, the floating-pins default.

**No continuous mode.** Each measurement is triggered by writing `CNTL1 =
0x01`; the part measures, sets `DRDY` and drops back to stand-by (§3.1.2).
Probe starts the first one, and every `compass_read()` that collects a
result starts the next — so reads never wait, and a read before the next
result is in returns **`-ENODATA`**. A measurement takes 5 ms, 6 ms with 16x
averaging (§3.1.1): read at 100 Hz or slower and every read is fresh. If a
started measurement has not landed after 100 ms (a lost trigger, or the data
taken through `ist8310_reg_read()`), the next read starts another; a trigger
the bus refused is retried on the very next read.

**What probe writes**: `0BH = 0x01` (soft reset, §6.4.7), `41H` averaging,
`42H = 0xC0` (pulse duration, §3.1.1's "please use this setting"),
`0AH = 0x01` (first measurement). The all-zero config is no averaging;
`examples/stm/ist8310` uses §3.1.1's low-noise `41H = 0x24`, 16 on every axis.

**No overflow flag.** `STAT2`'s `INT` trips on the *sum* of the axes past
1600 µT (§3.2), not on a clipped axis, so the driver checks the counts
instead: X or Y past ±5333 (1600 µT), or Z past the 14-bit rail ±8191 (§1;
2500 µT would be 8333), is `-ERANGE`.

**Gaps in the datasheet**, filled with a stated assumption:

- No soft-reset time. `SRST` "performs the POR routine", so the driver waits
  the POR time, 50 ms max (§4.5).
- `AVGCNTL`'s table calls code `010` the default while the register's reset
  column says 0; the driver always writes it, so it does not matter which.
- The temperature registers (`1CH`/`1DH`) have no scale in the datasheet, so
  there is no temperature call. Self-test (§3.1.3) is not modelled either;
  `ist8310_reg_write(c, 0x0C, 0x40)` turns it on.

The axes are drawn in §2.2's 3D top view: X+ and Y+ in the board plane, Z+ up
out of it. Set `mount` from that figure, and check it against a known
heading — nothing here has run on a chip.

### LIS3MDL

ST datasheet DocID024204 Rev 4, in `docs/` (PDFs are not committed). Every
register value in `src/lis3mdl_regs.h` and `src/lis3mdl.c` cites its section
or table.

**Two probes, one driver.** `lis3mdl_probe_i2c()` takes the same bus handle
as the other chips, at 0x1E (SA1 high) or 0x1C (SA1 low, §5.1.1);
`lis3mdl_probe_spi()` takes a `struct lis3mdl_spi_config`. On ESP-IDF that is
a host the app has `spi_bus_initialize()`d plus a CS pin, and the driver adds
the device in mode 3 at 5 MHz unless asked (at most 10 MHz, Table 6). On the
HAL it is an `SPI_HandleTypeDef *` the app has set up in mode 3 (§5.2: SPC
idles high, data captured on the rising edge), plus a GPIO port and pin for
chip select, an output idling high; the driver drives it around each
transfer. CS high selects I2C (§5), so on I2C tie it to Vdd_IO. On SPI there
is no acknowledge: an empty bus reads `WHO_AM_I` as 0x00 or 0xFF and the
probe says `-ENODEV`, not `-ENXIO`.

**The address byte is the only difference.** On I2C, `SUB(7)` set
auto-increments (§5.1.1); on SPI the byte is RW, MS, AD(5:0) (§5.2), so a
burst read of the six data bytes is `0xA8` on I2C and `0xE8` on SPI.

**What probe writes**, after `WHO_AM_I` reads 0x3D (Table 17), every control
register from scratch, conversion last: `CTRL_REG1` with `TEMP_EN`, the X/Y
mode (Table 21) and the rate (Table 22); `CTRL_REG2` the full scale
(Table 25); `CTRL_REG4` the Z mode (Table 31); `CTRL_REG5 = 0x40`, `BDU`, so a
burst never mixes two samples; `CTRL_REG3 = 0x00`, continuous conversion and
4-wire SPI. All zero is the chip's own defaults: ±4 gauss, 10 Hz, low-power.
`enum lis3mdl_odr` puts 10 Hz first for that reason, so its values are not
the register codes. Remove writes `CTRL_REG3 = 0x03`, power-down (Table 28).

**Clipping.** Table 3's measurement range times its sensitivity is about
27370 counts on every range; past it, `compass_read()` is `-ERANGE`. The
part's own flag, `MROI` in `INT_SRC` (Table 38), sits in the interrupt block
the driver leaves off.

**Gaps in the datasheet**, filled with a stated assumption:

- No time for `SOFT_RST` or `REBOOT` to finish. The driver uses neither:
  writing every control register gives a known state without a wait.
- The temperature has a slope, 8 LSB/°C (Table 4), but no zero point, so
  `lis3mdl_temperature()` is relative, like the QMC5883L's.
- `FAST_ODR` rates (Table 20) and the self-test (`ST`) are not modelled;
  `lis3mdl_reg_write()` reaches them.

The axes are drawn in Figure 5's top view. Set `mount` from it, and check
against a known heading — nothing here has run on a chip.

### CMPS14

Robot Electronics' "CMPS14 – documentation", in `docs/cmps14.pdf` (not
committed). Every command byte in `src/cmps14_cmds.h` and `src/cmps14.c`
cites its page.

**Serial only.** The module has an I2C mode too, chosen by its mode pin at
power-up (p.1); this driver does not support it. Tie the mode pin to 0 V
before power-up, and set the UART up as p.5 says: **9600 bps, 8 data bits,
no parity, 1 stop bit**, at 3.3–5 V logic. Never RS232 levels. On ESP-IDF
`cmps14_probe()` takes a `uart_port_t` whose driver the app has installed;
on the HAL a `UART_HandleTypeDef *` it has initialised. The line is the
app's; the driver never configures it.

**Command and answer.** Every exchange is one command byte and a fixed
number of answer bytes (p.6), which is exactly one bus `transfer`. Before
each one the port drops whatever is waiting on the line, so a late byte from
an earlier exchange is never read as this one's answer. Probe sends
`GET VERSION` (0x11): silence within 100 ms is `-ENXIO`, decided before
anything is allocated. The module has no identity register, so any
one-byte answer counts as a CMPS14.

**The field is already calibrated.** `GET MAGNETOMETER` (0x19) answers six
bytes, high byte first, in **µT with Q point 4** — 16 counts per µT (p.3,
p.6) — corrected for hard and soft iron by the module itself (p.3), so skip
your own hard-iron offset unless you have turned its calibration off. The
module reports no overflow and the document gives no range, so a CMPS14
read is never `-ERANGE`.

**Extras.** `cmps14_bearing_deg()` is the module's own tilt-compensated
bearing (0x13, 0–3599 tenths, p.2). `cmps14_calibration_state()` (0x24)
gives two bits each for magnetometer, accelerometer, gyro and system (p.7;
the gyro field is a known sensor bug there). `cmps14_set_calibration()`
sends 0x98, 0x95, 0x99, each answered 0x55, then the setup byte with bit 7
set and your `CMPS14_CAL_*` bits (p.7). `cmps14_store_calibration()` sends
0xF0, 0xF5, 0xF6 and `cmps14_erase_calibration()` 0xE0, 0xE5, 0xE2, then
waits the 300 ms p.7 recommends for the BNO080's reset. `cmps14_command()`
reaches the rest of p.6: pitch, roll, acceleration, gyro.

**Gaps in the document**, filled with a stated assumption:

- No answer time. The driver allows 100 ms per exchange; six bytes take
  about 6 ms at 9600 bps.
- p.7 gives the calibration setup byte no answer. The driver waits for one
  byte anyway: 0x55 or silence is success, anything else `-EIO`. A late
  0x55 left unread would land in the next command's answer.
- The protocol has no checksum. Stale input is dropped before each command,
  best effort: an answer that comes after its own timeout can still shift
  the next one.
- Changing the baud rate (0xA0, 0xA1) is not modelled: the answer comes
  back at the new rate (p.7), so the app must follow with its own UART.
  `cmps14_command()` sends it.

## Adding a chip

A driver is a probe, a remove and one read function:

1. `include/foo.h`: `foo_probe(struct compass_dev *compass, <bus>, const struct foo_config *cfg)`
   with `<bus>` `i2c_master_bus_handle_t` under `#ifdef ESP_PLATFORM` and
   `I2C_HandleTypeDef *` otherwise, as `qmc5883p.h` does (an SPI chip takes a
   per-platform `struct foo_spi_config`, as `lis3mdl.h` does), plus
   `void foo_remove(struct compass_dev *compass);`. Give the config a
   `uint32_t scl_hz` under `#ifdef ESP_PLATFORM`.
2. `src/foo.c`, flat in `src/` — stm-forge does not glob subdirectories —
   with private headers beside it. Wrap the whole driver in
   `#if CONFIG_COMPASS_FOO` (never `#ifdef`). It names a platform only to
   unpack an SPI config: define an I2C probe with `compass_i2c_bus bus`, keep
   a `struct compass_bus` in your state, and talk to the chip only through
   `bus.ops->transfer()` or `compass_reg_read()`/`compass_reg_write()` from
   `src/compass_priv.h`. Give it a
   `static const struct compass_ops foo_ops = { .read = foo_read };`, where
   `foo_read()` fills `field[]` in µT, chip frame, and returns `-ERANGE` for a
   clipped reading. It must not touch `t_us` or apply `mount`;
   `compass_read()` does both.
3. In probe: return `-EBUSY` if `compass->ops` or `compass->priv` is already
   set, before touching anything. Then set `compass->name` so failures log
   with it, refuse a bad config, check presence with `compass_i2c_probe()`
   before allocating (→ `-ENXIO`, logged with `compass_dbg()` only), allocate
   your state, `compass_i2c_attach()` (or `_spi_`, `_uart_`) it, with
   `compass_config_scl_hz(cfg)` on I2C, put it into `compass->priv`, set
   `compass->ops = &foo_ops`, and on failure `compass_bus_detach()` and put
   both back to NULL; remove detaches too. Use `compass_sleep_ms()` for
   every wait, `compass_expired()` for every poll limit, and `compass_err()`
   for anything the user needs to see.
4. Guard your chip-specific calls with `compass->ops == &foo_ops`.
5. Add `CONFIG_COMPASS_FOO` to the no-chip checks in `CMakeLists.txt`,
   `src/compass_port_stm.c` and `src/compass_priv.h`, a `src/foo.c` line to
   `CMakeLists.txt`, a `config COMPASS_FOO` bool to `Kconfig`,
   `# COMPASS_FOO is not set` to both forges' defconfigs, and
   `test/test_foo.c` targets to `test/Makefile`.

## What it does not do

- **No calibration.** Hard- and soft-iron offsets are yours; see above.
- **No tilt compensation.** That needs an accelerometer; see the imu library.
- **No interrupts, FIFO or DMA.** Data ready is polled over the bus, and every
  transfer is a blocking bus call.
- **No HMC5883L.** The gy271 example will tell you if you have one.
- **No locking.** One thread per `compass_dev` and per bus handle, or bring
  your own mutex. The HAL's own lock answers a clash with `HAL_BUSY`, which
  comes back as `-EAGAIN`.
- **No bus setup.** Pins, clocks and speed are the app's.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. The chip drivers are tested
once, through `src/compass_port_stm.c` against the slice of the HAL stubbed
in `test/hal_stubs/`; `test_port_stm` drives all three bus kinds of that
port directly, and `test_port_esp` checks what `src/compass_port_esp.c` does
differently, against `test/idf_stubs/`. Nothing is checked against real
silicon. The `compass: <chip>:` lines in the output are the drivers logging
the failures the tests provoke.

| Binary | Checks |
|---|---|
| `test_core` | `compass_read()` stamps before the read, rotates by `mount` (and not its transpose), passes errno through and leaves the sample alone on error; the default clock is `HAL_GetTick()` in µs; `compass_heading_deg()` at the four cardinal points, wrap into [0, 360) for any declination including the float edge at 360; log prefix and `compass_log_sample()` format, including 64-bit timestamps without `%lld` |
| `test_qmc5883l_math` | the sensitivities against Linux's milligauss-per-LSB and the datasheet's LSB-per-gauss forms, full scale fits int16, little-endian samples, rate and oversampling tables, register constants |
| `test_qmc5883l` | a fake QMC5883L behind a stubbed HAL: absent (`-ENXIO`, nothing touched, debug line only), floating bus and wrong identity (`-ENODEV`), `-EAGAIN`/`-ETIMEDOUT`/`-EIO` mapping, probe register state, the reset wait never rounded down, overflow as `-ERANGE`, range change keeps continuous mode, standby, temperature, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |
| `test_qmc5883p_math` | Table 2 sensitivities, 30000-count full scale on every range equal to the OVFL threshold, §7.1/§7.2/§7.3/§7.4 register bytes from the field builders, §7.3's self-test signs, register constants typed from the datasheet |
| `test_ist8310_math` | §4.4's 0.3 µT per count against the 3.3 LSB/µT it rounds to, the ±1600 µT and 14-bit limits on every axis and both signs, little-endian samples, §6.4.10 averaging codes and §3.1.1's 24h, register constants typed from the datasheet |
| `test_ist8310` | a fake IST8310 that measures only when triggered and lands the result 6 ms later: probe's reset, wait and §3.1.1 settings, `-ENXIO`/`-ENODEV`/errno mapping, a read before the result is `-ENODATA` and never re-triggers a running measurement, each read starts the next, clipped axes are `-ERANGE`, a lost trigger is recovered after more than 100 ms and a refused one on the next read, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |
| `test_port_esp` | the ESP-IDF port: every `esp_err_t` to its errno through probe, read and write; the device added at 400 kHz for `scl_hz` 0 and at the given speed otherwise, from the port and through a probe; the device removed by `*_remove()`, by a probe that fails after adding it, and never left behind by a refused add; `compass_time_us()`/`compass_tick_ms()` from `esp_timer`; the tick-rounded sleep at 100 Hz and 1 kHz; the `compass` log tag; I2C, SPI and UART framing — a bare command, write-then-read refused on I2C, SPI as one full-duplex transaction with rx after cmd and tx, UART input flushed before the send, a short reply as `-ETIMEDOUT`, driver failures as `-EIO`, an uninstalled UART driver refused; the LIS3MDL's SPI device in mode 3 at 5 MHz by default, 10 MHz allowed and 10 MHz + 1 refused, removed on remove and on a wrong `WHO_AM_I`; the CMPS14 refusing a port with no driver installed, probing by `GET VERSION`, and silence as `-ENXIO` |
| `test_cmps14_math` | p.6's high-byte-first samples, Q point 4 (16 counts per µT), 0–3599 tenths of a degree, every command byte and p.7's setup-byte bits typed from the document |
| `test_cmps14` | a fake module on the HAL UART that answers the p.6 table and the p.7 sequences only in order: probe by `GET VERSION`, silence as `-ENXIO` with nothing allocated, a stale byte on the line dropped before the version is read, the field in µT, a module gone quiet as `-ETIMEDOUT` with the sample untouched, bearing and calibration state, the calibration setup byte with bit 7 set and undefined bits refused, its answer read whether it comes at once, late (during the wait, where unread it would shift the next read) or not at all, any other answer as `-EIO`, store, erase with its 300 ms, an answer other than 0x55 as `-EIO`, the escape hatch, `-EBUSY`, foreign devices refused |
| `test_port_stm` | the HAL port's three ops: HAL status to errno on each; I2C reads as `Mem_Read`, writes as `Mem_Write`, a bare command as a plain transmit, and the transfers I2C cannot do refused before the bus; SPI chip select low around cmd, tx and rx and high again after a failure; UART overrun cleared and stale bytes read out before the send, no receive after a failed send or for a command without a reply, a short reply as `-ETIMEDOUT` |
| `test_lis3mdl_math` | Table 3 ranges and sensitivities, 100 µT per gauss of counts on every range, Table 22 rates and the enum's mapping onto them with 10 Hz first, the clip limit on every axis and sign, little-endian samples, Table 4's 8 LSB/°C, register addresses and bit positions typed from the datasheet |
| `test_lis3mdl` | a fake LIS3MDL on I2C and on SPI that decodes the address byte as §5.1.1 and §5.2 say, so a missing auto-increment or read bit gives wrong data, and counts writes to reserved registers or must-be-0 bits: probe's register state for the defaults and a full config, the same field read through either bus, `-ERANGE` past full scale with the sample untouched, absent on I2C (`-ENXIO`, nothing allocated), wrong `WHO_AM_I` and an empty SPI bus (`-ENODEV`, nothing leaked), chip select released after every transfer and every failure, extras, the escape hatch's limits, `-EBUSY`, foreign devices refused |
| `test_qmc5883p` | a fake QMC5883P behind a stubbed HAL that counts every datasheet rule broken: probe's exact write sequence against §7.1 and §7.2, suspend between modes, 29H before measuring, STATUS clears on read and the latched overflow survives a `data_ready()` poll, self-test only in continuous mode with the 5 ms wait honoured, range and mode restored, stale DRDY/OVFL cleared, a self-test that never sees data times out after more than 100 ms by the tick — across the tick counter's wrap — and still restores, errno mapping, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic`, with
`-Wl,--wrap` (GNU ld) to count allocations and capture `printf()`.

## License

MIT
