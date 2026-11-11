#include "buf.h"
#include <esp_log.h>
#include <string.h>

#define BUF_SIZE 1024

static const char *TAG = "MAIN";

void app_main(void) {
  uint8_t test_no = 0;

  //
  // T1. Create / Write / Read / Fill
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  buf_t buf = buf_create(BUF_SIZE);
  uint8_t tab[100] = {0};
  buf_write(buf, tab, sizeof(tab));

  size_t fill = buf_get_fill(buf);
  ESP_LOGI(TAG, "fill=%d", fill);

  size_t item_size;
  void *ptr = buf_read_up_to(buf, &item_size, sizeof(tab));
  ESP_LOGI(TAG, "item_size=%d", item_size);

  fill = buf_get_fill(buf);
  ESP_LOGI(TAG, "fill=%d", fill);

  buf_return_item(buf, ptr);
  fill = buf_get_fill(buf);
  ESP_LOGI(TAG, "fill=%d", fill);

  int err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }

  //
  // T2. Overrun
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  buf = buf_create(256);
  uint8_t big[300] = {0};
  bool ret = buf_write(buf, big, sizeof(big));
  if (!ret) {
    ESP_LOGI(TAG, "overrun! cannot write!");
  }
  buf_diag_t diag = buf_get_diag(buf);
  ESP_LOGI(TAG, "overrun cnt=%d occupancy=%d", diag.overrun_cnt,
           diag.occupancy);

  err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }

  //
  // T3. Underrun
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  buf = buf_create(128);
  ptr = buf_read_up_to(buf, &item_size, 100);
  if (!ptr) {
    ESP_LOGI(TAG, "underrun! cannot read");
  }
  diag = buf_get_diag(buf);
  ESP_LOGI(TAG, "underrun cnt=%d occupancy=%d", diag.underrun_cnt,
           diag.occupancy);

  err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }

  //
  // T4. Flush
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  buf = buf_create(BUF_SIZE);
  uint8_t data[128] = {0};

  ret = buf_write(buf, data, sizeof(data));
  if (ret) {
    fill = buf_get_fill(buf);
    ESP_LOGI(TAG, "fill=%d", fill);
  }
  err = buf_flush(buf);
  if (err == 0) {
    fill = buf_get_fill(buf);
    ESP_LOGI(TAG, "fill=%d after flush", fill);
  }

  // T5. Thresholds
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  ret = buf_write(buf, data, sizeof(data));
  if (ret) {
    fill = buf_get_fill(buf);
    ESP_LOGI(TAG, "fill=%d", fill);
  }
  if (buf_over_thr(buf, 64)) {
    ESP_LOGI(TAG, "over OK!");
  }
  if (buf_below_thr(buf, 256)) {
    ESP_LOGI(TAG, "below OK!");
  }
  if (!buf_below_thr(buf, 64)) {
    ESP_LOGI(TAG, "not below OK!");
  }
  err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }

  // T6. Wrap Around
  ESP_LOGI(TAG, "test no.%d", ++test_no);
  buf = buf_create(BUF_SIZE);
  uint8_t chunk[128] = {0};
  for (int i = 0; i < 1000; i++) {
    buf_write(buf, chunk, sizeof(chunk));
    size_t sz;
    void *p = buf_read_up_to(buf, &sz, sizeof(chunk));
    buf_return_item(buf, p);
  }
  diag = buf_get_diag(buf);
  ESP_LOGI(TAG,
           "max fill=%d occupancy=%d overrun=%d underrun=%d written=%d read=%d",
           diag.max_fill, diag.occupancy, diag.overrun_cnt, diag.underrun_cnt,
           diag.written, diag.read);

  ESP_LOGI(TAG, "reset diag!");
  buf_reset_diag(buf);
  diag = buf_get_diag(buf);
  ESP_LOGI(TAG,
           "max fill=%d occupancy=%d overrun=%d underrun=%d written=%d read=%d",
           diag.max_fill, diag.occupancy, diag.overrun_cnt, diag.underrun_cnt,
           diag.written, diag.read);

  err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }

  //
  // T7. Simple test
  uint8_t in[4] = {1, 2, 3, 4};
  uint8_t out[6] = {0};
  buf = buf_create(8);

  ret = buf_write(buf, in, sizeof(in));
  diag = buf_get_diag(buf);
  ESP_LOGI(TAG,
           "max fill=%d occupancy=%d overrun=%d underrun=%d written=%d read=%d",
           diag.max_fill, diag.occupancy, diag.overrun_cnt, diag.underrun_cnt,
           diag.written, diag.read);

  ret = buf_write(buf, in, sizeof(in));
  size_t sz;
  void *p = buf_read_up_to(buf, &sz, sizeof(out));

  memcpy(out, p, sz);

  for (int i = 0; i < sizeof(out); i++) {
    ESP_LOGI(TAG, "out[%d]=%d", i, *(out + i));
  }
  buf_return_item(buf, p);

  diag = buf_get_diag(buf);
  ESP_LOGI(TAG,
           "max fill=%d occupancy=%d overrun=%d underrun=%d written=%d read=%d",
           diag.max_fill, diag.occupancy, diag.overrun_cnt, diag.underrun_cnt,
           diag.written, diag.read);

  ret = buf_write(buf, in, sizeof(in));
  p = buf_read_up_to(buf, &sz, sizeof(out));

  memcpy(out, p, sz);

  for (int i = 0; i < sizeof(out); i++) {
    ESP_LOGI(TAG, "out[%d]=%d", i, *(out + i));
  }
  buf_return_item(buf, p);

  diag = buf_get_diag(buf);
  ESP_LOGI(TAG,
           "max fill=%d occupancy=%d overrun=%d underrun=%d written=%d read=%d",
           diag.max_fill, diag.occupancy, diag.overrun_cnt, diag.underrun_cnt,
           diag.written, diag.read);

  err = buf_destroy(buf);
  if (err == 0) {
    ESP_LOGI(TAG, "destroy OK!");
  }
}
