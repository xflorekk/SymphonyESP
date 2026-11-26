#ifndef BUF_H
#define BUF_H

#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <stdbool.h>

typedef struct {
  volatile uint32_t overrun_cnt;
  volatile uint32_t underrun_cnt;
  volatile size_t max_fill;
  volatile size_t occupancy;
  volatile uint32_t written;
  volatile uint32_t read;
} buf_diag_t;

typedef struct buf_s {
  RingbufHandle_t handle;
  size_t capacity;
  buf_diag_t diag;
} *buf_t;

buf_t buf_create(size_t size);
int buf_destroy(buf_t buf);
size_t buf_get_fill(buf_t buf);
bool buf_write(buf_t buf, const uint8_t *data, size_t len);
bool buf_over_thr(buf_t buf, size_t thr);
bool buf_below_thr(buf_t buf, size_t thr);

int buf_flush(buf_t buf);
void *buf_read_up_to(buf_t buf, size_t *item_size, size_t max_size);
void buf_return_item(buf_t buf, void *data);
buf_diag_t buf_get_diag(buf_t buf);
int buf_reset_diag(buf_t buf);

#endif