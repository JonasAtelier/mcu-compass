# Hardware identification and wiring

[Back to README](../README.md)

Paths mentioned in this guide are relative to the repository root.

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
