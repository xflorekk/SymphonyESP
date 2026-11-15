/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BUF_H
#define BUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Buffer handle
 *
 * Handle is opaque. Struct is defined only in buf.c,
 * user has no direct access to handle, capacity or counters.
 * Access only via buf_ functions.
 */
typedef struct buf_s *buf_t;

/**
 * @brief Buffer error codes
 */
typedef enum {
  BUF_OK = 0,               /*!< Success*/
  BUF_ERR_INVALID_ARG = -1, /*!< NULL or incorrect size argument*/
  BUF_ERR_NO_MEM = -2,      /*!< Not enough memory to create the buffer*/
  BUF_ERR_FULL = -3,        /*!< Buffer full, nothing has been written*/
  BUF_ERR_EMPTY = -4,       /*!< Buffer empty, nothing has been read*/
} buf_err_t;

/**
 *@brief Copy of buffer diagnostics
 *
 * It is a snapshot, not a pointer to inside of the buffer,
 * its value is returned by buf_get_diag(), caller cannot damage the counters
 */
typedef struct {
  volatile uint32_t
      overrun_cnt; /*!< Number of write attempts when buffer was full*/
  volatile uint32_t
      underrun_cnt;         /*!< Number of read attempts when buffer was empty*/
  volatile size_t max_fill; /*!< Maximum fill ever observed*/
  volatile size_t occupancy; /*!< Fill at the moment of reading diagnostics*/
  volatile uint32_t written; /*!< Total bytes written*/
  volatile uint32_t read;    /*!< Total bytes read*/
} buf_diag_t;

/**
 *@brief Creates buffer
 *
 *@param[in] size Buffer capacity in bytes
 *
 *@return Buffer handle or NULL in case of an error
 *
 */
buf_t buf_create(size_t size);

/**
 *@brief Destroys buffer
 *
 *@param[in] buf Buffer handle
 *
 *@return
 *      - BUF_OK when success
 *      - BUF_ERR_INVALID_ARG when buf == NULL
 */
buf_err_t buf_destroy(buf_t buf);

/**
 *@brief Number of bytes currently in the buffer
 *
 *@param[in] buf Buffer handle
 *
 *@return Fill in bytes, 0 if buf == NULL
 */
size_t buf_get_fill(buf_t buf);

/**
 *@brief Amount of free space in the buffer
 *
 *@param[in] buf Buffer handle
 *
 *@return Free space in bytes, 0 if buf == NULL
 */
size_t buf_get_free(buf_t buf);

/**
 *@brief Buffer Capacity
 *
 *@param[in] buf Buffer handle
 *
 *@return Buffer capacity in bytes, 0 if buf == NULL
 */
size_t buf_get_capacity(buf_t buf);

/**
 *@brief Atomic write - either all or nothing
 *
 *@param[in] buf Buffer handle
 *@param[in] data Data to be written
 *@param[in] len How many bytes to write
 *
 *@note Use when one item has to be written in full, e.g. MP3 chunk
 *
 *@return
 *      -BUF_OK all data has been written
 *      -BUF_ERR_FULL not enough space
 *      -BUF_ERR_INVALID_ARG invalid argument
 */
buf_err_t buf_write(buf_t buf, const uint8_t *data, size_t len);

/**
 *@brief Partial write - write as much as fits
 *
 *@param[in] buf Buffer handle
 *@param[in] data Data to be written
 *@param[in] len How many bytes to write
 *
 *@note Use for PCM streaming, when you can write the rest later.
 *      If it returns less than len, drain the buffer
 *      and write the remainder.
 *
 *@return How many bytes have been written, 0 if buffer is full or invalid
 *      argument
 */
size_t buf_write_partial(buf_t buf, const uint8_t *data, size_t len);

/**
 *@brief Read with copy - copies to out and frees the item
 *
 *@param[in] buf Buffer handle
 *@param[out] out Destination buffer
 *@param[in] max_len Maximum number of bytes to copy
 *
 *@note Intended for decoder, no need to call buf_return_item().
 *
 *@return How many bytes have been copied, 0 if empty or invalid argument
 */
size_t buf_read_copy(buf_t buf, uint8_t *out, size_t max_len);

/**
 *@brief Checks whether fill is at or above the threshold
 *
 *@param[in] buf Buffer handle
 *@param[in] thr Threshold in bytes
 *
 *@return true if fill >= thr
 */
bool buf_over_thr(buf_t buf, size_t thr);

/**
 *@brief Checks whether fill is below the threshold
 *
 *@param[in] buf Buffer handle
 *@param[in] thr Threshold in bytes
 *
 *@return true if fill < thr
 */
bool buf_below_thr(buf_t buf, size_t thr);

/**
 *@brief Flushes the buffer - discards all data
 *
 *@param[in] buf Buffer handle
 *
 *@return
 *      -BUF_OK on success
 *      -BUF_ERR_INVALID_ARG if buf == NULL
 */
buf_err_t buf_flush(buf_t buf);

/**
 *@brief Zero-copy read - returns the pointer to data inside the buffer
 *
 *@param[in] buf Buffer handle
 *@param[out] item_size Length of returned item in bytes
 *@param[in] max_size Maximum number of bytes to read
 *
 *@note After use you MUST call buf_return_item(), otherwise the buffer
 *      will remain occupied and block further writes.
 *@note Use only for I2S drain, where you do not want a copy.
 *
 *@return
 *      -Pointer to data, *item_size filled
 *      -NULL if empty, *item_size untouched
 */
void *buf_read_up_to(buf_t buf, size_t *item_size, size_t max_size);

/**
 *@brief Returns an item, previously obtained with buf_read_up_to()
 *
 *@param[in] buf Buffer handle
 *@param[in] data Pointer returned by buf_read_up_to()
 *
 *@return
 *      -BUF_OK on success
 *      -BUF_ERR_INVALID_ARG if invalid argument
 */
buf_err_t buf_return_item(buf_t buf, void *data);

/**
 *@brief Returns a copy of diagnostics
 *
 *@param[in] buf Buffer handle
 *
 *@return Copy of buf_diag_t, zeroed struct if buf == NULL
 */
buf_diag_t buf_get_diag(buf_t buf);

/**
 *@brief Reset diagnostic data
 *
 *@param[in] buf Buffer handle
 *
 *@return
 *      -BUF_OK on success
 *      -BUF_ERR_INVALID_ARG if buf == NULL
 */
buf_err_t buf_reset_diag(buf_t buf);

#ifdef __cplusplus
}
#endif

#endif // BUF_H