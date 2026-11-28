/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DEC_H
#define DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 *@brief Decoder handle
 *
 * Handle is opaque. Struct is defined only in dec.c,
 * user has no direct access to decoder handle, info or counters.
 * Access only via dec_* functions.
 */
typedef struct dec_s *dec_t;

/**
 *@brief Decoder error codes
 */
typedef enum {
  DEC_OK = 0,                 /*!< Success */
  DEC_ERR_INVALID_ARG = -1,   /*!< NULL handle or invalid argument */
  DEC_ERR_NO_MEM = -2,        /*!< Not enough memory */
  DEC_ERR_NOT_SUPPORTED = -3, /*!< Codec not supported */
  DEC_ERR_DECODE = -4,        /*!< Decoder returned an error */
  DEC_ERR_FAIL = -5,          /*!< ESP audio codec driver failed */
} dec_err_t;

/**
 *@brief Supported codecs
 */
typedef enum {
  DEC_CODEC_MP3 = 0, /*!< MP3 */
  DEC_CODEC_AAC,     /*!< AAC, ADTS */
  DEC_CODEC_FLAC,    /*!< FLAC */
  DEC_CODEC_WAV,     /*!< WAV */
  DEC_CODEC_OPUS,    /*!< Opus */
  DEC_CODEC_PCM,     /*!< Raw PCM, needs sample_rate / channels / bits in cfg */
  DEC_CODEC_AUTO,    /*!< Auto-detect from stream header */
} dec_codec_t;

/**
 *@brief Decoder configuration
 */
typedef struct {
  dec_codec_t codec; /*!< Codec to decode */
  uint32_t
      sample_rate;  /*!< Only for PCM, e.g. 44100. Ignored for other codecs. */
  uint8_t channels; /*!< Only for PCM, 1 or 2. Ignored for other codecs. */
  uint8_t bits;     /*!< Only for PCM, e.g. 16. Ignored for other codecs. */
} dec_cfg_t;

/**
 *@brief Decoded stream info
 *
 * Filled by decoder after successfully decoded frame.
 * Before first frame all fields are 0
 */
typedef struct {
  uint32_t sample_rate; /*!< Sample rate in Hz, e.g. 44100 */
  uint8_t channels;     /*!< Channels, 1 or 2 */
  uint8_t bits;         /*!< Bits per sample, e.g. 16 */
  uint32_t bitrate;     /*!< Bitrate in bps, 0 if unknown */
} dec_info_t;

/**
 *@brief Copy of decoder diagnostics
 *
 * Snapshot, not pointer to the inside. Returned by value from dec_get_diag().
 */
typedef struct {
  uint32_t frame_cnt;   /*!< Number of successfully decoded frames */
  uint32_t err_cnt;     /*!< Number of failed dec_decode() calls */
  uint32_t bytes_in;    /*!< Total bytes fed to decoder */
  uint32_t bytes_out;   /*!< Total PCM bytes produced */
  uint32_t sample_rate; /*!< Last known sample rate from dec_get_info() */
  uint8_t channels;     /*!< Last known channels from dec_get_info() */
} dec_diag_t;

/**
 *@brief Registers decoder
 *
 *@note Calls for
 *      -esp_audio_dec_register_default()
 *      -esp_audio_simple_dec_register_default()
 */
void dec_register_default(void);

/**
 *@brief Creates a decoder
 *
 *@param[in] cfg Pointer to configuration, must not be NULL
 *
 *@return Handle or NULL in case of an error
 */
dec_t dec_create(const dec_cfg_t *cfg);

/**
 *@brief Destroys a decoder
 *
 *@param[in] dec Decoder handle
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if dec == NULL
 */
dec_err_t dec_destroy(dec_t dec);

/**
 *@brief Changes codec of an existing decoder
 *
 *@param[in] dec Decoder handle
 *@param[in] codec New codec
 *
 *@note Destroys old decoder handle and creates a new one.
 *      Pending buffer is cleared.
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if dec == NULL
 *      -DEC_ERR_NOT_SUPPORTED if codec is not supported
 *      -DEC_ERR_FAIL if ESP audio codec failed
 */

dec_err_t dec_set_codec(dec_t dec, dec_codec_t codec);

/**
 *@brief Low-level decode - one call, no pending handling
 *
 *@param[in] dec Decoder handle
 *@param[in] in Input encoded data
 *@param[in] in_len Length of input in bytes
 *@param[out] out Output PCM buffer, int16_t
 *@param[in] out_cap Capacity of out in bytes
 *@param[out] consumed How many bytes of input were consumed
 *@param[out] produced How many PCM bytes were produced
 *
 *@note If consumed < in_len, caller must keep the remainder and
 *      feed it again on next call. For player use dec_process()
 *      instead, which does it internally.
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if invalid argument
 *      -DEC_ERR_DECODE if decoder failed
 */
dec_err_t dec_decode(dec_t dec, const uint8_t *in, size_t in_len, uint8_t *out,
                     size_t out_cap, size_t *consumed, size_t *produced);

/**
 *@brief High-level decode for player - handles pending internally
 *
 *@param[in] dec Decoder handle
 *@param[in] in Input encoded data, may be NULL, if in_len == 0 to flush pending
 *@param[in] in_len Length of input in bytes
 *@param[out] out Output PCM buffer, int16_t
 *@param[in] out_cap Capacity of out in bytes
 *@param[out] produced How many PCM bytes were produced
 *
 *@note Pending bytes that decoder did not consume are kept inside dec_t.
 *       Caller does not need to manage consumed / pending.
 *
 *@return
 *      - DEC_OK on success
 *      - DEC_ERR_INVALID_ARG if invalid argument
 *      - DEC_ERR_DECODE if decoder failed
 */
dec_err_t dec_process(dec_t dec, const uint8_t *in, size_t in_len, int16_t *out,
                      size_t out_cap, size_t *produced);

/**
 *@brief Resets decoder state
 *
 *@param[in] dec Decoder handle
 *
 *@note Clears pending buffer and resets decoder, keeps codec and config.
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if dec == NULL
 */
dec_err_t dec_reset(dec_t dec);

/**
 *@brief Flushes decoder
 *
 *@param[in] dec Decoder handle
 *
 *@note Discards pending bytes without decoding.
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if dec == NULL
 */
dec_err_t dec_flush(dec_t dec);

/**
 *@brief Returns current stream info
 *
 *@param[in] dec Decoder handle
 *
 *@return Copy of dec_info_t, zeroed struct if dec == NULL
 */
dec_info_t dec_get_info(dec_t dec);

/**
 *@brief Returns a copy of diagnostics
 *
 *@param[in] dec Decoder handle
 *
 *@return Copy of dec_diag_t, zeroed struct if dec == NULL
 */
dec_diag_t dec_get_diag(dec_t dec);

/**
 *@brief Resets diagnostic counters
 *
 *@param[in] dec Decoder handle
 *
 *@return
 *      -DEC_OK on success
 *      -DEC_ERR_INVALID_ARG if dec == NULL
 */
dec_err_t dec_reset_diag(dec_t dec);

#ifdef __cplusplus
}
#endif

#endif // DEC_H
