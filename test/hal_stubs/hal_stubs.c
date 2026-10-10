/* The stub definitions every test binary links. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stm32_hal_stub.h"

uint32_t stub_tick;
char stub_log_last[256];

uint32_t HAL_GetTick(void)
{
	return stub_tick;
}

/*
 * compass_port_stm.c is in every binary, and it calls these. A test with a
 * fake chip defines its own, which win over these weak ones; a test that
 * reaches the bus without one has a bug, so stop it there.
 */
__attribute__((weak)) void HAL_Delay(uint32_t Delay)
{
	(void)Delay;
	abort();
}

__attribute__((weak)) HAL_StatusTypeDef
HAL_I2C_IsDeviceReady(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
		      uint32_t Trials, uint32_t Timeout)
{
	(void)hi2c;
	(void)DevAddress;
	(void)Trials;
	(void)Timeout;
	abort();
}

__attribute__((weak)) HAL_StatusTypeDef
HAL_I2C_Mem_Read(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
		 uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
		 uint16_t Size, uint32_t Timeout)
{
	(void)hi2c;
	(void)DevAddress;
	(void)MemAddress;
	(void)MemAddSize;
	(void)pData;
	(void)Size;
	(void)Timeout;
	abort();
}

__attribute__((weak)) HAL_StatusTypeDef
HAL_I2C_Mem_Write(I2C_HandleTypeDef *hi2c, uint16_t DevAddress,
		  uint16_t MemAddress, uint16_t MemAddSize, uint8_t *pData,
		  uint16_t Size, uint32_t Timeout)
{
	(void)hi2c;
	(void)DevAddress;
	(void)MemAddress;
	(void)MemAddSize;
	(void)pData;
	(void)Size;
	(void)Timeout;
	abort();
}

int __wrap_printf(const char *fmt, ...);
int __wrap_printf(const char *fmt, ...)
{
	va_list ap;
	size_t n;

	va_start(ap, fmt);
	vsnprintf(stub_log_last, sizeof(stub_log_last), fmt, ap);
	va_end(ap);

	n = strlen(stub_log_last);
	if (n && stub_log_last[n - 1] == '\n')
		stub_log_last[n - 1] = '\0';
	return fprintf(stdout, "%s\n", stub_log_last);
}
