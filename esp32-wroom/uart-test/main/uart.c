/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uart.h"
#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

/**
 *@brief Private definition of UART handle
 *
 * Defined only here, not in uart.h
 */
struct uart_s {
  uart_cfg_t cfg;
  uart_diag_t diag;
  bool enabled;
  bool installed;
};

uart_t uart_create(const uart_cfg_t *cfg) {
  if (!cfg) {
    return NULL;
  }
  if (cfg->port < UART_NUM_0 || cfg->port >= UART_NUM_MAX) {
    return NULL;
  }
  if (cfg->baud_rate == 0 || cfg->rx_buf_size == 0 || cfg->tx_buf_size == 0) {
    return NULL;
  }

  uart_t u = (uart_t)calloc(1, sizeof(struct uart_s));
  if (!u) {
    return NULL;
  }
  u->cfg = *cfg;
  u->enabled = false;
  u->installed = false;
  return u;
}

uart_err_t uart_destroy(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  uart_disable(uart);
  free(uart);
  return UART_OK;
}

uart_err_t uart_enable(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  if (uart->enabled) {
    return UART_OK;
  }

  uart_config_t idf_cfg = {
      .baud_rate = (int)uart->cfg.baud_rate,
      .data_bits = uart->cfg.data_bits,
      .parity = uart->cfg.parity,
      .stop_bits = uart->cfg.stop_bits,
      .flow_ctrl = uart->cfg.flow_ctrl,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_param_config((uart_port_t)uart->cfg.port, &idf_cfg) != ESP_OK) {
    return UART_ERR_FAIL;
  }
  if (uart_set_pin((uart_port_t)uart->cfg.port, uart->cfg.tx_gpio,
                   uart->cfg.rx_gpio, uart->cfg.rts_gpio,
                   uart->cfg.cts_gpio) != ESP_OK) {
    return UART_ERR_FAIL;
  }
  if (uart_driver_install((uart_port_t)uart->cfg.port,
                          (int)uart->cfg.rx_buf_size,
                          (int)uart->cfg.tx_buf_size, 0, NULL, 0) != ESP_OK) {
    return UART_ERR_FAIL;
  }
  uart->installed = true;
  uart->enabled = true;
  return UART_OK;
}

uart_err_t uart_disable(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->installed) {
    uart->enabled = false;
    return UART_OK;
  }
  uart_driver_delete((uart_port_t)uart->cfg.port);
  uart->installed = false;
  uart->enabled = false;
  return UART_OK;
}

uart_err_t uart_write(uart_t uart, const uint8_t *data, size_t len,
                      size_t *written, uint32_t timeout_ms) {
  if (!uart || !data) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->enabled) {
    return UART_ERR_FAIL;
  }
  if (len == 0) {
    if (written) {
      *written = 0;
    }
    return UART_OK;
  }

  int n = uart_write_bytes(uart->cfg.port, data, len);
  if (n < 0) {
    uart->diag.tx_err_cnt++;
    if (written) {
      *written = 0;
    }
    return UART_ERR_FAIL;
  }

  uart->diag.tx_bytes += (uint32_t)n;
  if (written) {
    *written = (size_t)n;
  }
  if ((size_t)n < len) {
    return UART_ERR_TIMEOUT;
  }

  TickType_t timeout_ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  if (uart_wait_tx_done(uart->cfg.port, timeout_ticks) != ESP_OK) {
    return UART_ERR_TIMEOUT;
  }
  return UART_OK;
}

uart_err_t uart_read(uart_t uart, uint8_t *out, size_t cap, size_t *read,
                     uint32_t timeout_ms) {
  if (!uart || !out) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->enabled) {
    return UART_ERR_FAIL;
  }
  if (cap == 0) {
    if (read) {
      *read = 0;
    }
    return UART_OK;
  }
  TickType_t timeout_ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  int n = uart_read_bytes(uart->cfg.port, out, cap, timeout_ticks);
  if (n < 0) {
    uart->diag.rx_err_cnt++;
    if (read) {
      *read = 0;
    }
    return UART_ERR_FAIL;
  }
  if (n == 0) {
    uart->diag.timeout_cnt++;
    if (read) {
      *read = 0;
    }
    return UART_ERR_TIMEOUT;
  }
  uart->diag.rx_bytes += (uint32_t)n;
  if (read) {
    *read = (size_t)n;
  }
  return UART_OK;
}

uart_err_t uart_flush_rx_buf(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->enabled) {
    return UART_ERR_FAIL;
  }
  if (uart_flush_input(uart->cfg.port) != ESP_OK) {
    return UART_ERR_FAIL;
  }
  return UART_OK;
}

uart_err_t uart_flush_tx_buf(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->enabled) {
    return UART_ERR_FAIL;
  }
  if (uart_wait_tx_done(uart->cfg.port, pdMS_TO_TICKS(100)) != ESP_OK) {
    return UART_ERR_TIMEOUT;
  }
  return UART_OK;
}

uart_err_t uart_set_baud_rate(uart_t uart, uint32_t baud_rate) {
  if (!uart || baud_rate == 0) {
    return UART_ERR_INVALID_ARG;
  }
  if (!uart->enabled) {
    return UART_ERR_FAIL;
  }
  if (uart_set_baudrate(uart->cfg.port, baud_rate) != ESP_OK) {
    return UART_ERR_FAIL;
  }
  uart->cfg.baud_rate = baud_rate;
  return UART_OK;
}

uart_diag_t uart_get_diag(uart_t uart) {
  uart_diag_t empty = {0};
  if (!uart) {
    return empty;
  }
  return uart->diag;
}

uart_err_t uart_reset_diag(uart_t uart) {
  if (!uart) {
    return UART_ERR_INVALID_ARG;
  }
  memset(&uart->diag, 0, sizeof(uart->diag));
  return UART_OK;
}