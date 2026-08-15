// A FreeRTOS queue stub that actually QUEUES.
//
// The shared hostcheck stub is compile-only: xQueueSend always succeeds and
// xQueueReceive always fails. That is enough to type-check CommandDispatcher and
// nothing else — a queue that never fills cannot show what happens when it does,
// and "the queue was full" is half of the publish transaction's failure matrix.
//
// This one is a bounded single-threaded ring holding the pointer-sized items the
// dispatcher stores. Single-threaded on purpose: the harness drives the two web
// callbacks in whatever interleaving it wants to test, which is more controllable
// than real threads and makes the failures reproducible.
#pragma once
#include <freertos/FreeRTOS.h>

#include <cstring>
#include <deque>
#include <vector>

struct GmbFakeQueue {
    size_t itemSize = 0;
    size_t capacity = 0;
    std::deque<std::vector<unsigned char>> items;
};

typedef GmbFakeQueue* QueueHandle_t;

inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t itemSize) {
    GmbFakeQueue* q = new GmbFakeQueue();
    q->capacity = len;
    q->itemSize = itemSize;
    return q;
}

inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
    if (!q || q->items.size() >= q->capacity) return pdFALSE;
    std::vector<unsigned char> bytes(q->itemSize);
    std::memcpy(bytes.data(), item, q->itemSize);
    q->items.push_back(std::move(bytes));
    return pdTRUE;
}

inline BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t) {
    if (!q || q->items.empty()) return pdFALSE;
    std::memcpy(out, q->items.front().data(), q->itemSize);
    q->items.pop_front();
    return pdTRUE;
}

inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) {
    return q ? static_cast<UBaseType_t>(q->items.size()) : 0;
}
