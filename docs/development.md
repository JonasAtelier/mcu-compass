# Architecture, driver development and tests

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

## Repository layout

```
include/                    public API, the only directory apps include
  compass.h                 struct compass_dev, compass_read(), compass_time_us(), heading
  compass_log.h             compass_err/warn/info/dbg, compass_log_sample()
  qmc5883l.h qmc5883p.h ist8310.h lis3mdl.h cmps14.h
                            one header per chip: probe, remove, extras
src/                        private to the library
  compass.c                 the core: timestamp, ops.read, mount rotation
  compass_log.c             compass_log_sample()
  compass_priv.h            bus ops, attach functions, waits; shared by all chips
  compass_port_esp.c        that header on ESP-IDF; empty elsewhere
  compass_port_stm.c        that header on the STM32 HAL; empty under ESP-IDF
  qmc5883l/                 qmc5883l.c  qmc5883l_regs.h  qmc5883l_math.h
  qmc5883p/                 qmc5883p.c  qmc5883p_regs.h  qmc5883p_math.h
  ist8310/                  ist8310.c   ist8310_regs.h   ist8310_math.h
  lis3mdl/                  lis3mdl.c   lis3mdl_regs.h   lis3mdl_math.h
  cmps14/                   cmps14.c    cmps14_proto.h   cmps14_math.h
test/                       host tests, `make -C test test`
  test_<chip>_math.c        maths tests: no bus and no platform API
  esp/                      suites on idf_stubs/, through compass_port_esp.c
  stm/                      suites on hal_stubs/, through compass_port_stm.c
  idf_stubs/  hal_stubs/    the slice of ESP-IDF and of the HAL the code calls
examples/
  esp/<chip-or-board>/      ESP-IDF projects: gy271, qmc5883l, qmc5883p
  stm/<chip-or-board>/      STM32 snippets: gy271, ist8310, qmc5883l, qmc5883p
docs/                       the five guides; datasheet PDFs here are gitignored
assets/                     README figures
CMakeLists.txt              ESP-IDF only; stm-forge globs src/**/*.c instead
Kconfig                     COMPASS and COMPASS_<CHIP>, for ESP-IDF and both forges' menus
idf_component.yml           ESP-IDF component manifest
```

Naming inside `src/<chip>/`, the same in every category library:

| File | Holds |
|---|---|
| `<chip>.c` | the driver: probe, remove, the `ops.read`, the chip's extras; may split into `<chip>_<part>.c` (e.g. `bmi088_bus.c`) |
| `<chip>_regs.h` | register map: addresses, bit fields, values, each with its datasheet citation |
| `<chip>_proto.h` | serial protocol: command bytes, frames, checksums, for parts without registers |
| `<chip>_math.h` | pure conversions, compiled and asserted on the host by `test/<chip>_math` tests |
| `<chip>_priv.h` | driver state, only if needed |

A chip has `_regs.h` or `_proto.h`, never both; `_math.h` and `_priv.h` only
when it needs them. Everything in `src/` is private: apps see `include/` only.

## Platform ports

It replaces esp-compass and stm-compass. The
chip drivers in `src/` are the same C on both; only the bus, the clock and
the sleep under them differ, in `src/compass_port_esp.c` (ESP-IDF
`i2c_master`, `esp_timer`, FreeRTOS) and `src/compass_port_stm.c` (HAL). Each
port is empty on the other platform, chosen by `ESP_PLATFORM`, which ESP-IDF
defines for every component. Probes take the platform's own bus handle:
`i2c_master_bus_handle_t` on ESP-IDF, `I2C_HandleTypeDef *` on the HAL. Only
the ESP-IDF configs have `scl_hz`, because only there does the driver add its
own device to the bus; on the HAL the bus speed is the handle's.

## Bus abstraction

Under the chips sits a regmap-style ops
table, `struct compass_bus_ops` in `src/compass_priv.h`: each chip holds one
`struct compass_bus` and talks to it only through `transfer(cmd, tx, rx)`.
The kind — I2C, SPI or UART — is picked at run time by which attach the
probe calls; the platform stays a link-time choice. `transfer` is a command
with a reply, not a register read/write, because a serial-command module
(command byte, optional arguments, a fixed-length answer) does not fit
register access; on I2C and SPI the command is the register address, and
`compass_reg_read()`/`compass_reg_write()` are the usual shape on top. The
CMPS14 is the UART user: each of its serial commands is one `transfer`.

## Adding a chip

A driver is a probe, a remove and one read function:

1. `include/foo.h`: `foo_probe(struct compass_dev *compass, <bus>, const struct foo_config *cfg)`
   with `<bus>` `i2c_master_bus_handle_t` under `#ifdef ESP_PLATFORM` and
   `I2C_HandleTypeDef *` otherwise, as `qmc5883p.h` does (an SPI chip takes a
   per-platform `struct foo_spi_config`, as `lis3mdl.h` does), plus
   `void foo_remove(struct compass_dev *compass);`. Give the config a
   `uint32_t scl_hz` under `#ifdef ESP_PLATFORM`.
2. `src/foo/foo.c`, in a folder of its own with its private headers
   (`foo_regs.h`, `foo_math.h`); common files and the platform ports stay at
   the top of `src/`. Include siblings and `compass_priv.h` by plain name,
   never with `../`: siblings resolve from the chip's own folder, and
   `compass_priv.h` through `src/`, which the ESP-IDF CMakeLists has as
   `PRIV_INCLUDE_DIRS` and stm-forge's `libs.cmake` puts on this library's
   include path while it compiles every `.c` under `src/`. Wrap the whole
   driver in
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
   `src/compass_port_stm.c` and `src/compass_priv.h`, a `src/foo/foo.c` line to
   `CMakeLists.txt`, a `config COMPASS_FOO` bool to `Kconfig`,
   `# COMPASS_FOO is not set` to both forges' defconfigs, and
   `test/stm/test_foo.c` (and `test/test_foo_math.c` for its maths header)
   targets to `test/Makefile`.

## Tests

```sh
make -C test test
```

Host-only; no toolchain, HAL or ESP-IDF needed. The layout is the one in
[Repository layout](#repository-layout). The chip drivers are tested once,
in `test/stm/`, through `src/compass_port_stm.c` against the slice of the HAL
stubbed in `test/hal_stubs/`; `stm/test_port_stm` drives all three bus kinds
of that port directly, and `esp/test_port_esp` checks what
`src/compass_port_esp.c` does differently, against `test/idf_stubs/`. The
maths tests in `test/` touch no bus or platform API; two of them include a
public header, so they build with the HAL stub's flags. Nothing is checked
against real silicon. The `compass: <chip>:` lines in the output are the
drivers logging the failures the tests provoke.

| Binary | Checks |
|---|---|
| `stm/test_core` | `compass_read()` stamps before the read, rotates by `mount` (and not its transpose), passes errno through and leaves the sample alone on error; the default clock is `HAL_GetTick()` in µs; `compass_heading_deg()` at the four cardinal points, wrap into [0, 360) for any declination including the float edge at 360; log prefix and `compass_log_sample()` format, including 64-bit timestamps without `%lld` |
| `test_qmc5883l_math` | the sensitivities against Linux's milligauss-per-LSB and the datasheet's LSB-per-gauss forms, full scale fits int16, little-endian samples, rate and oversampling tables, register constants |
| `stm/test_qmc5883l` | a fake QMC5883L behind a stubbed HAL: absent (`-ENXIO`, nothing touched, debug line only), floating bus and wrong identity (`-ENODEV`), `-EAGAIN`/`-ETIMEDOUT`/`-EIO` mapping, probe register state, the reset wait never rounded down, overflow as `-ERANGE`, range change keeps continuous mode, standby, temperature, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |
| `test_qmc5883p_math` | Table 2 sensitivities, 30000-count full scale on every range equal to the OVFL threshold, §7.1/§7.2/§7.3/§7.4 register bytes from the field builders, §7.3's self-test signs, register constants typed from the datasheet |
| `test_ist8310_math` | §4.4's 0.3 µT per count against the 3.3 LSB/µT it rounds to, the ±1600 µT and 14-bit limits on every axis and both signs, little-endian samples, §6.4.10 averaging codes and §3.1.1's 24h, register constants typed from the datasheet |
| `stm/test_ist8310` | a fake IST8310 that measures only when triggered and lands the result 6 ms later: probe's reset, wait and §3.1.1 settings, `-ENXIO`/`-ENODEV`/errno mapping, a read before the result is `-ENODATA` and never re-triggers a running measurement, each read starts the next, clipped axes are `-ERANGE`, a lost trigger is recovered after more than 100 ms and a refused one on the next read, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |
| `esp/test_port_esp` | the ESP-IDF port: every `esp_err_t` to its errno through probe, read and write; the device added at 400 kHz for `scl_hz` 0 and at the given speed otherwise, from the port and through a probe; the device removed by `*_remove()`, by a probe that fails after adding it, and never left behind by a refused add; `compass_time_us()`/`compass_tick_ms()` from `esp_timer`; the tick-rounded sleep at 100 Hz and 1 kHz; the `compass` log tag; I2C, SPI and UART framing — a bare command, write-then-read refused on I2C, SPI as one full-duplex transaction with rx after cmd and tx, UART input flushed before the send, a short reply as `-ETIMEDOUT`, driver failures as `-EIO`, an uninstalled UART driver refused; the LIS3MDL's SPI device in mode 3 at 5 MHz by default, 10 MHz allowed and 10 MHz + 1 refused, removed on remove and on a wrong `WHO_AM_I`; the CMPS14 refusing a port with no driver installed, probing by `GET VERSION`, and silence as `-ENXIO` |
| `test_cmps14_math` | p.6's high-byte-first samples, Q point 4 (16 counts per µT), 0–3599 tenths of a degree, every command byte and p.7's setup-byte bits typed from the document |
| `stm/test_cmps14` | a fake module on the HAL UART that answers the p.6 table and the p.7 sequences only in order: probe by `GET VERSION`, silence as `-ENXIO` with nothing allocated, a stale byte on the line dropped before the version is read, the field in µT, a module gone quiet as `-ETIMEDOUT` with the sample untouched, bearing and calibration state, the calibration setup byte with bit 7 set and undefined bits refused, its answer read whether it comes at once, late (during the wait, where unread it would shift the next read) or not at all, any other answer as `-EIO`, store, erase with its 300 ms, an answer other than 0x55 as `-EIO`, the escape hatch, `-EBUSY`, foreign devices refused |
| `stm/test_port_stm` | the HAL port's three ops: HAL status to errno on each; I2C reads as `Mem_Read`, writes as `Mem_Write`, a bare command as a plain transmit, and the transfers I2C cannot do refused before the bus; SPI chip select low around cmd, tx and rx and high again after a failure; UART overrun cleared and stale bytes read out before the send, no receive after a failed send or for a command without a reply, a short reply as `-ETIMEDOUT` |
| `test_lis3mdl_math` | Table 3 ranges and sensitivities, 100 µT per gauss of counts on every range, Table 22 rates and the enum's mapping onto them with 10 Hz first, the clip limit on every axis and sign, little-endian samples, Table 4's 8 LSB/°C, register addresses and bit positions typed from the datasheet |
| `stm/test_lis3mdl` | a fake LIS3MDL on I2C and on SPI that decodes the address byte as §5.1.1 and §5.2 say, so a missing auto-increment or read bit gives wrong data, and counts writes to reserved registers or must-be-0 bits: probe's register state for the defaults and a full config, the same field read through either bus, `-ERANGE` past full scale with the sample untouched, absent on I2C (`-ENXIO`, nothing allocated), wrong `WHO_AM_I` and an empty SPI bus (`-ENODEV`, nothing leaked), chip select released after every transfer and every failure, extras, the escape hatch's limits, `-EBUSY`, foreign devices refused |
| `stm/test_qmc5883p` | a fake QMC5883P behind a stubbed HAL that counts every datasheet rule broken: probe's exact write sequence against §7.1 and §7.2, suspend between modes, 29H before measuring, STATUS clears on read and the latched overflow survives a `data_ready()` poll, self-test only in continuous mode with the 5 ms wait honoured, range and mode restored, stale DRDY/OVFL cleared, a self-test that never sees data times out after more than 100 ms by the tick — across the tick counter's wrap — and still restores, errno mapping, `-EBUSY`, foreign devices refused, a refused config allocates nothing and a failed probe leaks nothing |

All build under `-std=gnu11 -Wall -Wextra -Werror -pedantic`, with
`-Wl,--wrap` (GNU ld) to count allocations and capture `printf()`.
