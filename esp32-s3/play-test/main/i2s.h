/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef I2S_H
#define I2S_H

#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief I2S handle
 *
 * Handle is opaque. Struct is defined only in i2s.c,
 * user has no direct access to channel handle or config.
 * Access only via i2s_* functions.
 */
typedef struct i2s_s *i2s_t;

/**
 *@brief I2S error codes
 */
typedef enum {
  I2S_OK = 0,
  I2S_ERR_INVALID_ARG = -1,
  I2S_ERR_NO_MEM = -2,
  I2S_ERR_NOT_SUPPORTED = -3,
  I2S_ERR_FAIL = -4,
} i2s_err_t;

/**
 *@brief I2S configuration
 */
typedef struct {
  int port;                  /*!< I2S port, e.g. I2S_NUM_0 */
  i2s_role_t role;           /*!< I2S_ROLE_MASTER / I2S_ROLE_SLAVE */
  bool is_tx;                /*!< true = TX (DAC), false = RX (BT) */
  bool use_apll;             /*!< Hint: prefer APLL if chip supports it.
                                  On ESP32-S3 it is ignored, PLL_160M is used. */
  int mclk_gpio;             /*!< MCLK GPIO or GPIO_NUM_NC */
  int bclk_gpio;             /*!< BCLK GPIO */
  int ws_gpio;               /*!< WS / LRCK GPIO */
  int dout_gpio;             /*!< DOUT GPIO for TX, GPIO_NUM_NC for RX */
  int din_gpio;              /*!< DIN GPIO for RX, GPIO_NUM_NC for TX */
  uint32_t rate;             /*!< Sample rate in Hz, e.g. 44100 */
  i2s_data_bit_width_t bits; /*!< I2S_DATA_BIT_WIDTH_16BIT etc. */
  i2s_slot_mode_t slot_mode; /*!< I2S_SLOT_MODE_STEREO / MONO */
  uint32_t dma_buf_count;    /*!< Number of DMA buffers */
  uint32_t dma_buf_len;      /*!< Length of each DMA buffer in samples */
} i2s_cfg_t;

/**
 *@brief Copy of I2S diagnostics
 *
 * Snapshot, not a pointer to the inside. Returned by value from i2s_get_diag().
 */
typedef struct {
  uint32_t write_cnt;     /*!< Number of successful i2s_write() calls */
  uint32_t write_bytes;   /*!< Total bytes written to I2S */
  uint32_t write_err_cnt; /*!< Number of failed i2s_write() calls */
  uint32_t read_cnt;      /*!< Number of successful i2s_read() calls */
  uint32_t read_bytes;    /*!< Total bytes read from I2S */
  uint32_t read_err_cnt;  /*!< Number of failed i2s_read() calls */
  uint32_t enable_cnt;    /*!< Number of i2s_enable() calls */
  uint32_t reconfig_cnt;  /*!< Number of i2s_reconfig_clk() calls */
  uint32_t rate;          /*!< Current sample rate in Hz */
  bool is_enabled;        /*!< true if channel is enabled */
} i2s_diag_t;

/**
 *@brief Creates an I2S channel
 *
 *@param[in] cfg Pointer to configuration, must not be NULL
 *
 *@return Handle or NULL in case of an error
 */
i2s_t i2s_create(const i2s_cfg_t *cfg);

/**
 *@brief Reconfigures I2S format - rate + bits + channels
 *
 *@param[in] i2s I2S handle
 *@param[in] rate New sample rate in Hz, e.g. 44100
 *@param[in] num_bits Bits per sample as number, 16 or 24
 *@param[in] channels Channels as number, 1 or 2
 *
 *@note If all params are same as current, returns I2S_OK without touching
 *      driver. Validates num_bits / channels inside I2S, player does not need
 *      to know I2S enums.
 *
 *@return
 *      -I2S_OK on success
 *      -I2S_ERR_INVALID_ARG if i2s == NULL, rate == 0 or slot, bits are invalid
 *      -I2S_ERR_FAIL if ESP-IDF driver failed
 */
i2s_err_t i2s_reconfig(i2s_t i2s, uint32_t rate, uint8_t num_bits,
                       uint8_t slot_mode);

/**
 *@brief Reconfigures sample rate
 *
 *@param[in] i2s I2S handle
 *@param[in] rate New sample rate in Hz
 *
 *@note Disables channel, reconfigures clock, enables again.
 *
 *@return
 *      - I2S_OK on success
 *      - I2S_ERR_INVALID_ARG if i2s == NULL or rate == 0
 *      - I2S_ERR_FAIL if ESP-IDF driver failed
 */
i2s_err_t i2s_reconfig_clk(i2s_t i2s, uint32_t rate);

/**
 *@brief Writes PCM data to I2S
 *
 *@param[in] i2s I2S handle
 *@param[in] data PCM data
 *@param[in] len Length in bytes
 *@param[out] written How many bytes have been written
 *
 *@return
 *      -I2S_OK on success
 *      -I2S_ERR_INVALID_ARG if invalid argument
 *      -I2S_ERR_FAIL if driver failed
 */
i2s_err_t i2s_write(i2s_t i2s, const uint8_t *data, size_t len,
                    size_t *written);

/**
 *@brief Reads PCM data from I2S
 *
 *@param[in] i2s I2S handle
 *@param[out] out Destination buffer
 *@param[in] len Length to read in bytes
 *@param[out] read How many bytes have been read
 *
 *@return
 *      - I2S_OK on success
 *      - I2S_ERR_INVALID_ARG if invalid argument
 *      - I2S_ERR_FAIL if driver failed
 */
i2s_err_t i2s_read(i2s_t i2s, uint8_t *out, size_t len, size_t *read);

/**
 *@brief Enables I2S channel
 *
 *@param[in] i2s I2S handle
 *
 *@return I2S_OK on success, I2S_ERR_INVALID_ARG if i2s == NULL
 */
i2s_err_t i2s_enable(i2s_t i2s);

/**
 *@brief Disables I2S channel
 *
 *@param[in] i2s I2S handle
 *
 *@return I2S_OK on success, I2S_ERR_INVALID_ARG if i2s == NULL
 */
i2s_err_t i2s_disable(i2s_t i2s);

/**
 *@brief Destroys I2S channel
 *
 *@param[in] i2s I2S handle
 *
 *@return I2S_OK on success, I2S_ERR_INVALID_ARG if i2s == NULL
 */
i2s_err_t i2s_destroy(i2s_t i2s);

/**
 *@brief Returns a copy of diagnostics
 *
 *@param[in] i2s I2S handle
 *
 *@return Copy of i2s_diag_t, zeroed struct if i2s == NULL
 */
i2s_diag_t i2s_get_diag(i2s_t i2s);

/**
 *@brief Resets diagnostics counters
 *
 *@param[in] i2s I2S handle
 *
 *@return I2S_OK on success, I2S_ERR_INVALID_ARG if i2s == NULL
 */
i2s_err_t i2s_reset_diag(i2s_t i2s);

#ifdef __cplusplus
}
#endif

#endif // I2S_H