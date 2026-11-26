#include "m_buf.h"

ring_buf_handle_t ring_buf_create(size_t size) {
  return xRingbufferCreate(size, RINGBUF_TYPE_BYTEBUF);
}

size_t ring_buf_get_fill(ring_buf_handle_t buf, size_t size) {
  if (!buf) {
    return 0;
  }
  return size - xRingbufferGetCurFreeSize(buf);
}

bool ring_buf_over_thr(ring_buf_handle_t buf, size_t size, size_t thr) {
  // return xRingbufferGetCurFreeSize(buf) < (RING_BUF_SIZE - threshold);
  return ring_buf_get_fill(buf, size) > thr;
}

bool ring_buf_below_thr(ring_buf_handle_t buf, size_t size, size_t thr) {
  return ring_buf_get_fill(buf, size) < thr;
}

bool ring_buf_write(ring_buf_handle_t buf, const uint8_t *data, uint32_t len) {
  if (!buf) {
    return false;
  }
  return xRingbufferSend(buf, data, len, 0);
}

void *ring_buf_read_up_to(ring_buf_handle_t buf, size_t *item_size,
                          size_t max_size) {
  if (!buf)
    return NULL;
  return xRingbufferReceiveUpTo(buf, item_size, 0, max_size);
}

void ring_buf_return_item(ring_buf_handle_t buf, void *data) {
  if (buf && data) {
    vRingbufferReturnItem(buf, data);
  }
}