#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Host stub for the slice of the ESP-IDF spi_master API the port uses. */

typedef enum {
	SPI1_HOST = 0,
	SPI2_HOST = 1,
	SPI3_HOST = 2,
} spi_host_device_t;

typedef struct spi_device_t *spi_device_handle_t;

typedef struct {
	uint8_t mode;
	int clock_speed_hz;
	int spics_io_num;
	int queue_size;
} spi_device_interface_config_t;

typedef struct {
	size_t length;		/* in bits */
	const void *tx_buffer;
	void *rx_buffer;
} spi_transaction_t;

esp_err_t spi_bus_add_device(spi_host_device_t host_id,
			     const spi_device_interface_config_t *dev_config,
			     spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle,
				      spi_transaction_t *trans_desc);

#endif
