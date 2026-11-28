/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Maciek Pollok
 */

/**
 * @file play.h
 * @brief Player task: owns buf_in, buf_out, dec and i2s.
 *
 * Data flow: src -> buf_in -> dec -> buf_out -> i2s.
 * Control flow: app -> src -> player, ACK returns player -> src -> app.
 * Player holds pending, does backpressure and i2s reconfig.
 * Decoder only transforms in to out, it holds no pending.
 * Buffers are allocated in PSRAM, i2s DMA stays in internal RAM.
 */

#ifndef PLAY_H
#define PLAY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "i2s.h"

/**
 * @brief Opaque player handle.
 *
 * Player owns buf_in, buf_out, dec and i2s handles.
 * Created by play_create(), destroyed by play_destroy().
 */
typedef struct play_s *play_t;

/**
 * @brief Player error codes.
 */
typedef enum {
  PLAY_OK = 0,                   /**< Success */
  PLAY_ERR_INVALID_ARG = -1,     /**< NULL pointer or invalid value */
  PLAY_ERR_FAIL = -2,            /**< Generic failure */
  PLAY_ERR_NO_MEM = -3,          /**< PSRAM or queue allocation failed */
  PLAY_ERR_ALREADY_STARTED = -4, /**< Task already started */
  PLAY_ERR_NOT_STARTED = -5,     /**< Task not started */
  PLAY_ERR_TIMEOUT = -6,         /**< ACK not received in timeout_ms */
  PLAY_ERR_CONFIG_FAIL = -7,     /**< CONFIG failed, dec or i2s rejected */
  PLAY_ERR_DEC_FAIL = -8,        /**< Decoder fatal error */
  PLAY_ERR_I2S_FAIL = -9,        /**< I2S reconfig or write failed */
  PLAY_ERR_SRC_FAIL = -10,       /**< Src read failed or src not started */
  PLAY_ERR_INVALID_STATE = -11,  /**< Operation not allowed in current state */
  PLAY_ERR_PREBUFFER_TIMEOUT = -12 /**< Prebuffer threshold not reached */
} play_err_t;

/**
 * @brief Player states, private state machine.
 *
 * CONFIGURING is transient, never waits for data.
 * PREBUFFERING is a substate of PLAYING, not a separate state.
 */
typedef enum {
  PLAY_STATE_IDLE = 0,    /**< Created, waiting for CONFIG */
  PLAY_STATE_CONFIGURING, /**< dec_create + i2s_reconfig in progress */
  PLAY_STATE_PLAYING,     /**< Decoding and playing, includes prebuffering */
  PLAY_STATE_PAUSED,      /**< Decoding stopped, pending kept */
  PLAY_STATE_EOF,         /**< Src ended, dec flushed, rest played */
  PLAY_STATE_ERROR,       /**< Fatal error, only STOP recovers */
  PLAY_STATE_SHUTDOWN     /**< Cleanup in progress, task will exit */
} play_state_t;

/**
 * @brief Codec, player side.
 *
 * Player maps this to dec codec in play.c, play.h does not include dec.h.
 */
typedef enum {
  PLAY_CODEC_UNKNOWN = 0,
  PLAY_CODEC_MP3,
  PLAY_CODEC_AAC,
  PLAY_CODEC_FLAC,
  PLAY_CODEC_WAV,
  PLAY_CODEC_OPUS,
  PLAY_CODEC_PCM
} play_codec_t;

/**
 * @brief ACK bits confirming operation, player -> src -> app.
 *
 * Each operation has OK and FAIL bit.
 * Functions like play_config() wait for matching ACK with seq check.
 */
typedef enum {
  PLAY_ACK_CONFIG_OK = (1U << 0),   /**< dec_create + i2s_reconfig done */
  PLAY_ACK_CONFIG_FAIL = (1U << 1), /**< dec or i2s rejected config */
  PLAY_ACK_PLAY_OK = (1U << 2),     /**< Player entered PLAYING */
  PLAY_ACK_PLAY_FAIL = (1U << 3),   /**< Prebuffer or play failed */
  PLAY_ACK_PAUSE_OK = (1U << 4),    /**< Player entered PAUSED */
  PLAY_ACK_RESUME_OK = (1U << 5),   /**< Player back to PLAYING */
  PLAY_ACK_STOP_OK = (1U << 6),     /**< Player entered IDLE, buffers flushed */
  PLAY_ACK_EOF = (1U << 7),         /**< Src ended, dec flushed */
  PLAY_ACK_ERROR = (1U << 8),       /**< Player entered ERROR */
  PLAY_ACK_SHUTDOWN_OK = (1U << 9)  /**< Player cleaned PSRAM, task exiting */
} play_ack_t;

/**
 * @brief Player diagnostics data.
 */
typedef struct {
  uint32_t total_frames;  /**< Decoded frames */
  uint32_t decoded_bytes; /**< Bytes consumed from buf_in */
  uint32_t played_bytes;  /**< Bytes written to i2s */
  uint32_t underruns;     /**< buf_out empty, silence played */
  uint32_t overruns;      /**< dec produced more than i2s accepted */
  uint32_t reconfig_cnt;  /**< i2s reconfig count */
  uint32_t src_eof_cnt;   /**< EOF count */
  uint32_t err_cnt;       /**< Error count */
  play_err_t last_err;    /**< Last error code */
} play_diag_t;

/**
 * @brief Src read callback.
 *
 * @param src_ctx Src context, e.g. mem src, file src, net src.
 * @param buf Destination for encoded bytes.
 * @param len Max bytes to read.
 * @return Bytes read, 0 means EOF.
 *
 * Player does not know if src is flash, file, net or BT.
 * Src must not push data before player acked CONFIG.
 */
typedef size_t (*play_src_read_cb_t)(void *src_ctx, uint8_t *buf, size_t len);

/**
 * @brief Src seek callback, may be NULL if src is not seekable.
 *
 * @param src_ctx Src context.
 * @param pos Position in encoded stream.
 * @return true on success, false on failure.
 */
typedef bool (*play_src_seek_cb_t)(void *src_ctx, size_t pos);

/**
 * @brief State change callback, called from player task.
 *
 * @param user_ctx User context from play_cfg_t.
 * @param state New state.
 */
typedef void (*play_on_state_cb_t)(void *user_ctx, play_state_t state);

/**
 * @brief EOF callback, called from player task after dec flushed.
 *
 * @param user_ctx User context from play_cfg_t.
 */
typedef void (*play_on_eof_cb_t)(void *user_ctx);

/**
 * @brief Error callback, called from player task on fatal error.
 *
 * @param user_ctx User context from play_cfg_t.
 * @param err Error code.
 */
typedef void (*play_on_error_cb_t)(void *user_ctx, play_err_t err);

/**
 * @brief Player create config, given once at play_create().
 *
 * Buffers are allocated in PSRAM in play_create().
 * Task is created in play_start(), not in play_create().
 * I2S config is copied 1:1 from old main.c test, player owns i2s.
 * play_start() does i2s_create(&i2s_cfg) + i2s_enable().
 */
typedef struct {
  play_on_state_cb_t on_state; /**< State change callback, may be NULL */
  play_on_eof_cb_t on_eof;     /**< EOF callback, may be NULL */
  play_on_error_cb_t on_error; /**< Error callback, may be NULL */
  void *user_ctx;              /**< Passed to callbacks */
  uint32_t stack_size;      /**< 0 means default 8192, FLAC 24-bit use 12288 */
  uint32_t priority;        /**< 0 means default 5, higher than srv */
  uint32_t core_id;         /**< 0 or 1, 0 means default */
  uint32_t buf_in_size;     /**< 0 means default 32768, encoded bytes, PSRAM */
  uint32_t buf_out_size;    /**< 0 means default 65536, PCM bytes, PSRAM */
  uint32_t prebuffer_bytes; /**< 0 means default 8192, threshold for PLAYING */
  i2s_cfg_t i2s_cfg;        /**< I2S hardware config, 1:1 from old main.c */
} play_cfg_t;

/**
 * @brief Src config for player, given in play_set_src() and play_config().
 *
 * This is player side only: codec, bits, channels, rate and read callbacks.
 * URL, IP:port, BT setup belong to src task, not to player.
 */
typedef struct {
  play_codec_t codec; /**< Codec for dec */
  uint8_t bits;       /**< Bits per sample, 16, 24, 32 */
  uint8_t channels;   /**< Channels, 1 or 2 */
  uint32_t rate;      /**< Sample rate, 0 means unknown until first frame */
  play_src_read_cb_t src_read; /**< Encoded data source, must not be NULL */
  play_src_seek_cb_t src_seek; /**< Seek, may be NULL */
  void *src_ctx;               /**< Src context for callbacks */
} play_src_cfg_t;

/**
 * @brief Create player handle, allocate PSRAM buffers, no task yet.
 *
 * @param cfg Create config, must not be NULL.
 * @param out Output handle, must not be NULL.
 * @return PLAY_OK on success, error code otherwise.
 */
play_err_t play_create(const play_cfg_t *cfg, play_t *out);

/**
 * @brief Destroy player handle, free PSRAM buffers.
 *
 * @param play Player handle, may be NULL.
 * @return PLAY_OK on success, error code otherwise.
 * @note Call play_shutdown() before play_destroy() if task is running.
 */
play_err_t play_destroy(play_t play);

/**
 * @brief Start player task, state goes to IDLE.
 *
 * @param play Player handle.
 * @return PLAY_OK on success, PLAY_ERR_ALREADY_STARTED if already started.
 */
play_err_t play_start(play_t play);

/**
 * @brief Set new src, must be called after STOP, before CONFIG.
 *
 * @param play Player handle.
 * @param src_cfg Src config for player, must not be NULL.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_SRC_STARTED equivalent, error otherwise.
 * @note If old src was ON, app must shutdown it first, player does not own src
 * task.
 */
play_err_t play_set_src(play_t play, const play_src_cfg_t *src_cfg,
                        uint32_t timeout_ms);

/**
 * @brief Configure player with codec, bits, channels, rate.
 *
 * @param play Player handle.
 * @param src_cfg Src config for player, must not be NULL.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_CONFIG_OK, PLAY_ERR_CONFIG_FAIL if ACK_CONFIG_FAIL,
 *         PLAY_ERR_TIMEOUT if no ACK in timeout.
 * @note Sets need_i2s_cfg flag. If rate is 0, i2s reconfig waits for first
 * frame.
 */
play_err_t play_config(play_t play, const play_src_cfg_t *src_cfg,
                       uint32_t timeout_ms);

/**
 * @brief Start playback, wait for prebuffer threshold.
 *
 * @param play Player handle.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_PLAY_OK, PLAY_ERR_PREBUFFER_TIMEOUT if prebuffer
 * failed.
 */
play_err_t play_play(play_t play, uint32_t timeout_ms);

/**
 * @brief Pause playback, keep pending in buf_in and buf_out.
 *
 * @param play Player handle.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_PAUSE_OK, error otherwise.
 */
play_err_t play_pause(play_t play, uint32_t timeout_ms);

/**
 * @brief Resume playback after pause, no pending clear, no reconfig if format
 * same.
 *
 * @param play Player handle.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_RESUME_OK, error otherwise.
 */
play_err_t play_resume(play_t play, uint32_t timeout_ms);

/**
 * @brief Stop playback, flush buf_in and buf_out, dec reset, state to IDLE.
 *
 * @param play Player handle.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_STOP_OK, error otherwise.
 * @note STOP must succeed even from ERROR, force STOP on timeout.
 */
play_err_t play_stop(play_t play, uint32_t timeout_ms);

/**
 * @brief Shutdown player task, free is in play_destroy().
 *
 * @param play Player handle.
 * @param timeout_ms ACK wait timeout, UINT32_MAX means wait forever.
 * @return PLAY_OK if ACK_SHUTDOWN_OK, error otherwise.
 * @note Works from any state, even ERROR.
 */
play_err_t play_shutdown(play_t play, uint32_t timeout_ms);

/**
 * @brief Get current state, thread safe.
 *
 * @param play Player handle.
 * @param state Output state, must not be NULL.
 * @return PLAY_OK on success, error otherwise.
 */
play_err_t play_get_state(play_t play, play_state_t *state);

/**
 * @brief Get diagnostics snapshot, thread safe.
 *
 * @param play Player handle.
 * @param diag Output diag, must not be NULL.
 * @return PLAY_OK on success, error otherwise.
 */
play_err_t play_get_diag(play_t play, play_diag_t *diag);

/**
 * @brief Convert error code to string, for logs.
 *
 * @param err Error code.
 * @return Static string, never NULL.
 */
const char *play_err_to_str(play_err_t err);

#ifdef __cplusplus
}
#endif

#endif // PLAY_H