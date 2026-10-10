#ifndef TASK_H
#define TASK_H

#include "FreeRTOS.h"

/* Each test that links a driver defines both, to model its own clock. */
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#endif
