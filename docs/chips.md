# Chip reference

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## QMC5883P

QST datasheet QST-PD-B002-22 Rev E, in `docs/` (download it from qstcorp.com
if your clone lacks it; PDFs are not committed). Every register value in
`src/qmc5883p/qmc5883p_regs.h` and `src/qmc5883p/qmc5883p.c` cites its section or table.

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

## QMC5883L

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

## IST8310

iSentek datasheet Version 1.2, in `docs/` (LCSC carries it as part C2683055;
PDFs are not committed). Every register value in `src/ist8310/ist8310_regs.h` and
`src/ist8310/ist8310.c` cites its section.

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

## LIS3MDL

ST datasheet DocID024204 Rev 4, in `docs/` (PDFs are not committed). Every
register value in `src/lis3mdl/lis3mdl_regs.h` and `src/lis3mdl/lis3mdl.c` cites its section
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

## CMPS14

Robot Electronics' "CMPS14 – documentation", in `docs/cmps14.pdf` (not
committed). Every command byte in `src/cmps14/cmps14_cmds.h` and `src/cmps14/cmps14.c`
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
