// Minimal FreeRTOS queue stub for host compile-checking (NOT for running).
#pragma once
#include "FreeRTOS.h"
#include <cstddef>

typedef void* QueueHandle_t;

inline QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t) {
    return reinterpret_cast<QueueHandle_t>(1);
}
inline BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t) { return pdTRUE; }
inline BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t) { return pdFALSE; }
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t) { return 0; }
