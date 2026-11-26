#ifndef M_BUF_H
#define M_BUF_H

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include <stdbool.h>

#define RING_BUF_SIZE (96 * 1024)
#define RING_BUF_THRESHOLD (RING_BUF_SIZE / 2)

typedef RingbufHandle_t ring_buf_handle_t;

ring_buf_handle_t ring_buf_create(size_t size);
size_t ring_buf_get_fill(ring_buf_handle_t buf, size_t size);
bool ring_buf_over_thr(ring_buf_handle_t buf, size_t size, size_t thr);
bool ring_buf_below_thr(ring_buf_handle_t buf, size_t size, size_t thr);
bool ring_buf_write(ring_buf_handle_t buf, const uint8_t *data, uint32_t len);

void *ring_buf_read_up_to(ring_buf_handle_t buf, size_t *item_size,
                          size_t max_size);
void ring_buf_return_item(ring_buf_handle_t buf, void *data);

#endif