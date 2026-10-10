#ifndef STM32_HAL_STUB_H
#define STM32_HAL_STUB_H

/*
 * Host stub for the slice of the STM32 HAL this library uses, standing in
 * for "stm32f4xx.h" through CMSIS_device_header (see test/Makefile). Names,
 * signatures and values match the real F4/F7 HAL; the handle carries only an
 * identity. The tests define HAL_Delay() and the transfer functions, so each
 * fake chip keeps its own clock.
 */

#include <stdint.h>

typedef enum {
	HAL_OK		= 0x00,
	HAL_ERROR	= 0x01,
	HAL_BUSY	= 0x02,
	HAL_TIMEOUT	= 0x03,
} HAL_StatusTypeDef;

typedef struct {
	uint32_t id;
} I2C_HandleTypeDef;

#define I2C_MEMADD_SIZE_8BIT	0x00000001U

uint32_t HAL_GetTick(void);
void HAL_Delay(uint32_t Delay);

HAL_StatusTypeDef HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c,
					uint16_t DevAddress, uint32_t Trials,
					uint32_t Timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c,
				   uint16_t DevAddress, uint16_t MemAddress,
				   uint16_t MemAddSize, uint8_t *pData,
				   uint16_t Size, uint32_t Timeout);
HAL_StatusTypeDef HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c,
				    uint16_t DevAddress, uint16_t MemAddress,
				    uint16_t MemAddSize, uint8_t *pData,
				    uint16_t Size, uint32_t Timeout);

/* --- test hooks, defined in hal_stubs.c --------------------------------- */

/* What HAL_GetTick() returns, in ms; tests set it and their HAL_Delay() moves it. */
extern uint32_t stub_tick;

/*
 * The last printf() line, without its newline. The tests link with
 * -Wl,--wrap=printf, so every compass_* log line lands here as well as on
 * stdout.
 */
extern char stub_log_last[256];

#endif
