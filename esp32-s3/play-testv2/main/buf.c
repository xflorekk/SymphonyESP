/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "buf.h"
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief private definition of buf handle
 * Defined only here, not in buf.h. User has no direct access
 */
struct buf_s {
  RingbufHandle_t handle;
  size_t capacity;
  buf_diag_t diag;
};

buf_t buf_create(size_t size) {
  if (size == 0) {
    return NULL;
  }
  buf_t b = calloc(1, sizeof(*b));
  if (!b) {
    return NULL;
  }
  b->handle = xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
  if (!b->handle) {
    free(b);
    return NULL;
  }
  b->capacity = size;
  return b;
}

buf_err_t buf_destroy(buf_t buf) {
  if (!buf) {
    return BUF_ERR_INVALID_ARG;
  }
  if (buf->handle) {
    vRingbufferDelete(buf->handle);
  }
  free(buf);
  return BUF_OK;
}

size_t buf_get_fill(buf_t buf) {
  if (!buf) {
    return 0;
  }
  return buf->capacity - xRingbufferGetCurFreeSize(buf->handle);
}

size_t buf_get_free(buf_t buf) {
  if (!buf) {
    return 0;
  }
  return xRingbufferGetCurFreeSize(buf->handle);
}

buf_err_t buf_write(buf_t buf, const uint8_t *data, size_t len) {
  if (!buf || !data || len == 0) {
    return BUF_ERR_INVALID_ARG;
  }
  if (xRingbufferSend(buf->handle, data, len, 0) != pdTRUE) {
    buf->diag.overrun_cnt++;
    return BUF_ERR_FULL;
  }
  buf->diag.written += len;
  size_t fill = buf_get_fill(buf);
  if (fill > buf->diag.max_fill) {
    buf->diag.max_fill = fill;
  }
  return BUF_OK;
}

size_t buf_write_partial(buf_t buf, const uint8_t *data, size_t len) {
  if (!buf || !data || len == 0) {
    return 0;
  }
  size_t free = buf_get_free(buf);
  if (free == 0) {
    buf->diag.overrun_cnt++;
    return 0;
  }
  size_t to_write = len < free ? len : free;
  if (xRingbufferSend(buf->handle, data, to_write, 0) != pdTRUE) {
    buf->diag.overrun_cnt++;
    return 0;
  }
  buf->diag.written += to_write;
  size_t fill = buf_get_fill(buf);
  if (fill > buf->diag.max_fill) {
    buf->diag.max_fill = fill;
  }
  return to_write;
}

bool buf_over_thr(buf_t buf, size_t thr) {
  if (!buf) {
    return false;
  }
  return buf_get_fill(buf) >= thr;
}

bool buf_below_thr(buf_t buf, size_t thr) {
  if (!buf) {
    return false;
  }
  return buf_get_fill(buf) < thr;
}

buf_err_t buf_flush(buf_t buf) {
  if (!buf) {
    return BUF_ERR_INVALID_ARG;
  }
  size_t item_size;
  void *item;
  while ((item = xRingbufferReceiveUpTo(buf->handle, &item_size, 0, 1024)) !=
         NULL) {
    vRingbufferReturnItem(buf->handle, item);
  }
  return BUF_OK;
}

size_t buf_read_copy(buf_t buf, uint8_t *out, size_t max_len) {
  if (!buf || !out || max_len == 0) {
    return 0;
  }
  size_t item_size = 0;
  void *p = buf_read_up_to(buf, &item_size, max_len);
  if (!p) {
    return 0;
  }
  memcpy(out, p, item_size);
  buf_return_item(buf, p);
  return item_size;
}

void *buf_read_up_to(buf_t buf, size_t *item_size, size_t max_size) {
  if (!buf || !item_size) {
    return NULL;
  }
  void *p = xRingbufferReceiveUpTo(buf->handle, item_size, 0, max_size);
  if (!p) {
    buf->diag.underrun_cnt++;
  } else {
    buf->diag.read += *item_size;
  }
  return p;
}

buf_err_t buf_return_item(buf_t buf, void *data) {
  if (!buf || !data) {
    return BUF_ERR_INVALID_ARG;
  }
  vRingbufferReturnItem(buf->handle, data);
  return BUF_OK;
}

buf_diag_t buf_get_diag(buf_t buf) {
  buf_diag_t d = {0};
  if (!buf) {
    return d;
  }
  d = buf->diag;
  d.occupancy = buf_get_fill(buf);
  return d;
}

buf_err_t buf_reset_diag(buf_t buf) {
  if (!buf) {
    return BUF_ERR_INVALID_ARG;
  }
  buf->diag.overrun_cnt = 0;
  buf->diag.underrun_cnt = 0;
  buf->diag.max_fill = 0;
  buf->diag.written = 0;
  buf->diag.read = 0;
  return BUF_OK;
}