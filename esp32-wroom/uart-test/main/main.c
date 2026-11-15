#include "uart.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "main";

void app_main(void) {
  uart_cfg_t uart_cfg;
  uart_cfg = (uart_cfg_t){
      .port = UART_NUM_1,
      .baud_rate = 115200,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .tx_gpio = 17,
      .rx_gpio = 16,
      .rts_gpio = -1,
      .cts_gpio = -1,
      .rx_buf_size = 1024,
      .tx_buf_size = 1024,
  };

  uart_t uart = uart_create(&uart_cfg);
  if (!uart) {
    ESP_LOGE(TAG, "uart_create FAIL");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  uart_err_t err = uart_enable(uart);
  if (err != UART_OK) {
    ESP_LOGE(TAG, "uart_enable FAIL");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  ESP_LOGI(TAG, "uart OK, loopback test TX=%d RX=%d", uart_cfg.tx_gpio,
           uart_cfg.rx_gpio);

  uart_flush_rx_buf(uart);

  int i = 0;
  for (;;) {
    i++;
    const char *msg = "HELLO UART\n";
    size_t written = 0;

    err = uart_write(uart, (const uint8_t *)msg, strlen(msg), &written, 100);
    ESP_LOGI(TAG, "[%d] TX err=%d written=%u", i, err, (unsigned)written);

    uint8_t rx[256] = {0};
    size_t read = 0;
    err = uart_read(uart, rx, sizeof(rx) - 1, &read, 500);

    if (err == UART_OK) {
      ESP_LOGI(TAG, "[%d] RX OK %u bytes: %s", i, (unsigned)read, (char *)rx);
    } else {
      ESP_LOGW(TAG, "[%d] RX FAIL err=%d", i, err);
    }

    uart_diag_t d = uart_get_diag(uart);
    ESP_LOGI(TAG, "[%d] diag tx=%lu rx=%lu err_tx=%lu err_rx=%lu timeout=%lu",
             i, (unsigned long)d.tx_bytes, (unsigned long)d.rx_bytes,
             (unsigned long)d.tx_err_cnt, (unsigned long)d.rx_err_cnt,
             (unsigned long)d.timeout_cnt);

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
