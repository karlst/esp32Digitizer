/**
 * @file FreeRTOS.h
 * @brief Minimal FreeRTOS names needed to compile acquisition code on a PC.
 * Task/queue operations are implemented by nativeChecks.cpp using ordinary memory.
 * Critical-section and yield macros below do nothing because tests run one thread.
 * Passing these tests cannot prove cross-core locking or scheduler behavior.
 * pdMS_TO_TICKS treats a fake tick as one millisecond for deterministic timeouts.
 */
#pragma once
#include <cstdint>
#include <cstddef>
using BaseType_t = int;
using TaskHandle_t = void*;
using QueueHandle_t = void*;
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(value) (value)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portYIELD_FROM_ISR() ((void)0)
QueueHandle_t xQueueCreate(int length, size_t itemSize);
void vQueueDelete(QueueHandle_t queue);
int xQueueSend(QueueHandle_t queue, const void* item, int wait);
int xQueueReceive(QueueHandle_t queue, void* item, int wait);
int xTaskCreatePinnedToCore(void (*entry)(void*), const char*, int, void*, int, TaskHandle_t*, int);
uint32_t ulTaskNotifyTake(int clear, uint32_t wait);
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t* wake);
void vTaskDelay(uint32_t wait);
