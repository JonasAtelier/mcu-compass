# mcu-compass

Read a three-axis magnetic field in microteslas and calculate a heading. The QMC5883L, QMC5883P and IST8310 use I2C, the LIS3MDL uses I2C or SPI, and the CMPS14 supplies a tilt-compensated bearing over UART.

**Platforms** · ESP-IDF 5.2+ and STM32 HAL (F4/F7). Hardware testing is pending. The host tests use simulated devices.

## Quick start

Connect a GY-271 module over I2C. The example probes for the two supported QMC chips and identifies the unsupported HMC5883L.

In a terminal with ESP-IDF activated, get the example and select the target for an ESP32. Change the target if your board uses another ESP32 chip.

```sh
git clone https://github.com/JonasAtelier/mcu-compass.git compass
cd compass/examples/esp/gy271
idf.py set-target esp32
```

Check the [wiring guide](docs/hardware.md) and edit the settings in [main.c](examples/esp/gy271/main/main.c) to match your board. Connect the board by USB, then build, upload and open the serial monitor.

```sh
idf.py build flash monitor
```

On a supported board the monitor shows the chip name and field readings. Calibrate for magnetic offsets and scale, and compensate for tilt when calculating a heading from raw readings.

<details>
<summary>STM32 HAL example for an initialised bus</summary>

Initialise the bus in your application, then probe the sensor, wait for a
fresh measurement, and read a sample. This STM32 helper assumes the passed
I2C handle is already initialised. It polls for up to 200 ms, allowing time
for the QMC5883P's default 10 Hz measurement rate:

```c
#include <errno.h>
#include <stdbool.h>

#include "compass.h"
#include "compass_log.h"
#include "qmc5883p.h"

int read_once(I2C_HandleTypeDef *bus)
{
    struct compass_dev compass = { 0 }; /* Must start zeroed. */
    struct compass_sample sample;
    bool ready = false;
    int ret = qmc5883p_probe(&compass, bus, NULL);
    if (ret)
        return ret;

    for (unsigned int attempt = 0; attempt < 20; ++attempt) {
        HAL_Delay(10);
        ret = qmc5883p_data_ready(&compass, &ready);
        if (ret || ready)
            break;
    }
    if (!ret)
        ret = ready ? compass_read(&compass, &sample) : -ETIMEDOUT;
    if (!ret)
        compass_log_sample(&compass, &sample);

    qmc5883p_remove(&compass);
    return ret;
}
```

</details>

## Using the library in your project

- Add the component and enable the driver for your hardware. [Installation and build options](docs/installation.md)
- Start from an example for your chip. The STM32 examples fit into a project with board setup already in place. [ESP-IDF examples](examples/esp/) · [STM32 examples](examples/stm/)
- Look up calls and chip-specific settings. [API usage](docs/usage.md) · [Hardware reference](docs/chips.md)

To run the simulated tests on your computer, run `make -C test test` from the library root. [Test requirements and coverage](docs/development.md)

## License

[MIT](LICENSE)

<p align="center">
  <img src="assets/compass.svg"
       alt="A compass turning in the earth's field, and the two axes it produces"
       width="100%">
</p>
