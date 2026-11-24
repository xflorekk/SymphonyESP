/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UART_H
#define UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <driver/uart.h>
#include <stddef.h>
#include <stdint.h>

/**
 *@brief UART Handle
 *
 * Handle is opaque. Struct is defined only in uart.c,
 * user has no direct access to interface handle or config.
 * Access only via uart_* functions.
 */
typedef struct uart_s *uart_t;

/**
 *@brief UART error codes
 */
typedef enum {
  UART_OK = 0,                 /*!< Success */
  UART_ERR_INVALID_ARG = -1,   /*!< Invalid argument */
  UART_ERR_NO_MEM = -2,        /*!< Out of memory */
  UART_ERR_NOT_SUPPORTED = -3, /*!< Not supported */
  UART_ERR_TIMEOUT = -4,       /*!< Timeout, not a fatal error */
  UART_ERR_FAIL = -5,          /*!< Generic failure */
} uart_err_t;

/**
 *@brief UART configuration
 */
typedef struct {
  uart_port_t port; /*!< UART port, e.g. UART_NUM_1. UART_NUM_0 is for logging*/
  uint32_t baud_rate;              /*!< e.g. 115200 */
  uart_word_length_t data_bits;    /*!< e.g. UART_DATA_8_BITS */
  uart_parity_t parity;            /*!< e.g. UART_PARITY_DISABLE */
  uart_stop_bits_t stop_bits;      /*!< e.g. UART_STOP_BITS_1 */
  uart_hw_flowcontrol_t flow_ctrl; /*!< e.g. UART_HW_FLOWCTRL_DISABLE */
  int tx_gpio;                     /*!< TX pin, -1 = not used */
  int rx_gpio;                     /*!< RX pin, -1 = not used */
  int rts_gpio;                    /*!< RTS pin, -1 = not used */
  int cts_gpio;                    /*!< CTS pin, -1 = not used */
  size_t rx_buf_size;              /*!< RX ring buffer, e.g. 4096 */
  size_t tx_buf_size;              /*!< TX ring buffer, e.g. 4096 */
} uart_cfg_t;

/**
 *@brief UART diagnostics
 *
 * Counters are cumulative until uart_reset_diag().
 */
typedef struct {
  uint32_t tx_bytes;    /*!< Total bytes written */
  uint32_t rx_bytes;    /*!< Total bytes read */
  uint32_t tx_err_cnt;  /*!< TX errors */
  uint32_t rx_err_cnt;  /*!< RX errors, framing / parity / overrun */
  uint32_t timeout_cnt; /*!< Read timeouts, normal when task polls */
} uart_diag_t;

/**
 *@brief Create UART instance
 *
 *@param[in] cfg Configuration, must not be NULL
 *@return UART handle or NULL on failure
 */
uart_t uart_create(const uart_cfg_t *cfg);

/**
 *@brief Destroy UART instance
 *
 *@param[in] uart UART handle
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_destroy(uart_t uart);

/**
 *@brief Enable UART driver
 *
 *@param[in] uart UART handle
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_enable(uart_t uart);

/**
 *@brief Disable UART driver
 *
 *@param[in] uart UART handle
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_disable(uart_t uart);

/**
 *@brief Write data to UART
 *
 * Blocking until all bytes written or timeout.
 * Use it only from one TX task to avoid frame mixup
 *
 *@param[in] uart UART handle
 *@param[in] data Data to write
 *@param[in] len Length of data
 *@param[out] written Bytes actually written, may be NULL
 *@param[in] tiemout_ms Timeout in ms, e.g. 100
 *
 *@return
 *      -UART_OK on success
 *      -UART_ERR_INVALID_ARG
 *      -UART_ERR_TIMEOUT if timeout
 */
uart_err_t uart_write(uart_t uart, const uint8_t *data, size_t len,
                      size_t *written, uint32_t timeout_ms);

/**
 *@brief Read data from UART
 *
 *@param[in] uart UART handle
 *@param[out] out Output buffer
 *@param[in] cap Capacity of out buffer
 *@param[out] read Bytes actually read, may be NULL
 *@param[in] timeout_ms Timeout in ms, UINT32_MAX if infinite
 *
 *@return
 *      -UART_OK
 *      -UART_ERR_TIMEOUT
 *      -UART_ERR_INVALID_ARG
 */
uart_err_t uart_read(uart_t uart, uint8_t *out, size_t cap, size_t *read,
                     uint32_t timeout_ms);

/**
 *@brief Flush RX buffer
 *
 *@param[in] uart UART handle
 *
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_flush_rx_buf(uart_t uart);

/**
 *@brief Flush TX buffer
 *
 *@param[in] uart UART handle
 *
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_flush_tx_buf(uart_t uart);

/**
 *@brief Change baudrate at runtime
 *
 *@param[in] uart UART handle
 *@param[in] baud_rate New baudrate
 *
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_set_baud_rate(uart_t uart, uint32_t baud_rate);

/**
 *@brief Get a copy of diagnostics
 *
 *@param[in] uart UART handle
 *
 *@return Copy of diagnostics
 */
uart_diag_t uart_get_diag(uart_t uart);

/**
 *@brief Reset diagnostics counters
 *
 *@param[in] uart UART handle
 *
 *@return UART_OK on success, or error on fail
 */
uart_err_t uart_reset_diag(uart_t uart);

#ifdef __cplusplus
}
#endif

#endif // UART_H