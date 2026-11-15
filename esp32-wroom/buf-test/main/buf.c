#include "buf.h"
#include <stdlib.h>

buf_t buf_create(size_t size) {
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

int buf_destroy(buf_t buf) {
  if (!buf) {
    return -1;
  }
  if (buf->handle) {
    vRingbufferDelete(buf->handle);
  }
  free(buf);
  return 0;
}

bool buf_write(buf_t buf, const uint8_t *data, size_t len) {
  if (!buf) {
    return false;
  }
  if (xRingbufferSend(buf->handle, data, len, 0) != pdTRUE) {
    buf->diag.overrun_cnt++;
    return false;
  }
  buf->diag.written += len;
  size_t fill = buf_get_fill(buf);
  if (fill > buf->diag.max_fill) {
    buf->diag.max_fill = fill;
  }
  return true;
}

size_t buf_get_fill(buf_t buf) {
  if (!buf) {
    return 0;
  }
  return buf->capacity - xRingbufferGetCurFreeSize(buf->handle);
}

bool buf_over_thr(buf_t buf, size_t thr) {
  // return xRingbufferGetCurFreeSize(buf) < (buf_SIZE - threshold);
  return buf_get_fill(buf) > thr;
}

bool buf_below_thr(buf_t buf, size_t thr) {
  //
  return buf_get_fill(buf) < thr;
}

int buf_flush(buf_t buf) {
  if (!buf) {
    return -1;
  }
  size_t item_size;
  void *item;
  while ((item = xRingbufferReceiveUpTo(buf->handle, &item_size, 0, 1024)) !=
         NULL) {
    vRingbufferReturnItem(buf->handle, item);
  }
  return 0;
}

void *buf_read_up_to(buf_t buf, size_t *item_size, size_t max_size) {
  void *p = xRingbufferReceiveUpTo(buf->handle, item_size, 0, max_size);
  if (!p) {
    buf->diag.underrun_cnt++;
  } else {
    buf->diag.read += *item_size;
  }
  return p;
}

void buf_return_item(buf_t buf, void *data) {
  if (buf && data) {
    vRingbufferReturnItem(buf->handle, data);
  }
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

int buf_reset_diag(buf_t buf) {
  if (!buf) {
    return -1;
  }
  buf->diag.overrun_cnt = 0;
  buf->diag.underrun_cnt = 0;
  buf->diag.max_fill = 0;
  buf->diag.written = 0;
  buf->diag.read = 0;
  return 0;
}