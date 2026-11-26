/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Maciek Pollok
 */

/**
 * @file play.c
 * @brief Player task implementation, 1:1 with old main.c test logic.
 *
 * Player owns buf_in, buf_out, dec and i2s.
 * Uses buf.h, dec.h, i2s.h signatures from old main.c:
 * dec_decode(dec, in, in_len, out, out_cap, &consumed, &produced)
 * dec_get_info(dec) returns dec_info_t by value
 * i2s_write(dac, data, len, &written)
 * i2s_create(&cfg) + i2s_enable()
 * dec_set_codec() to switch codec without destroying dec.
 * Decoder has no NEED_MORE error, need-more = DEC_OK + consumed==0 +
 * produced==0
 */

#include "play.h"

#include "buf.h"
#include "dec.h"
#include "i2s.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "play";

#define PLAY_CMD_QUEUE_LEN 8
#define PLAY_DEFAULT_STACK (12 * 1024)
#define PLAY_DEFAULT_BUF_IN 65536
#define PLAY_DEFAULT_BUF_OUT 65536
#define PLAY_DEFAULT_PREBUFFER 8192
#define PLAY_TMP_IN_SIZE 32768
#define PLAY_TMP_PCM_SIZE 65536
#define PLAY_PENDING_SIZE 32768
#define PLAY_ENC_CHUNK 4096
#define PLAY_DECODE_POLL_MS 10

typedef enum {
  PLAY_CMD_SET_SRC = 0,
  PLAY_CMD_CONFIG,
  PLAY_CMD_PLAY,
  PLAY_CMD_PAUSE,
  PLAY_CMD_RESUME,
  PLAY_CMD_STOP,
  PLAY_CMD_SHUTDOWN
} play_cmd_type_t;

typedef struct {
  play_cmd_type_t type;
  uint32_t seq;
  play_src_cfg_t src_cfg;
  bool has_src_cfg;
} play_cmd_internal_t;

struct play_s {
  play_cfg_t cfg;
  play_src_cfg_t src_cfg;
  play_state_t state;
  play_diag_t diag;
  TaskHandle_t task_handle;
  QueueHandle_t cmd_q;
  EventGroupHandle_t ack_group;
  SemaphoreHandle_t lock;
  volatile bool task_should_exit;
  uint32_t seq;
  uint32_t ack_seq;
  play_ack_t ack_value;
  dec_t dec;
  i2s_t i2s;
  buf_t buf_in;
  buf_t buf_out;
  uint8_t *tmp_in;
  uint8_t *tmp_pcm;
  uint8_t *pending;
  size_t pending_len;
  uint32_t cur_rate;
  uint8_t cur_bits;
  uint8_t cur_ch;
  bool need_i2s_cfg;
  bool src_set;
  bool configured;
  bool src_eof;
  bool prebuffer_done;
};

static void play_set_state_locked(play_t play, play_state_t state) {
  play->state = state;
  if (play->cfg.on_state != NULL) {
    play->cfg.on_state(play->cfg.user_ctx, state);
  }
}

static void play_set_ack(play_t play, uint32_t seq, play_ack_t ack) {
  play->ack_seq = seq;
  play->ack_value = ack;
  xEventGroupSetBits(play->ack_group, (EventBits_t)ack);
}

static void play_clear_ack(play_t play) {
  xEventGroupClearBits(play->ack_group, 0xFFFFFF);
  play->ack_seq = 0;
  play->ack_value = 0;
}

static uint8_t *play_alloc_psram(size_t size) {
  uint8_t *p =
      (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == NULL) {
    p = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return p;
}

static dec_codec_t play_codec_to_dec(play_codec_t c) {
  switch (c) {
  case PLAY_CODEC_MP3:
    return DEC_CODEC_MP3;
  case PLAY_CODEC_AAC:
    return DEC_CODEC_AAC;
  case PLAY_CODEC_FLAC:
    return DEC_CODEC_FLAC;
  case PLAY_CODEC_WAV:
    return DEC_CODEC_WAV;
  case PLAY_CODEC_OPUS:
    return DEC_CODEC_OPUS;
  case PLAY_CODEC_PCM:
    return DEC_CODEC_PCM;
  default:
    return DEC_CODEC_MP3;
  }
}

static play_err_t play_send_cmd_and_wait(play_t play,
                                         const play_cmd_internal_t *cmd,
                                         play_ack_t ok_bit, play_ack_t fail_bit,
                                         uint32_t timeout_ms) {
  if (play == NULL || cmd == NULL) {
    return PLAY_ERR_INVALID_ARG;
  }
  if (xSemaphoreTake(play->lock, portMAX_DELAY) != pdTRUE) {
    return PLAY_ERR_FAIL;
  }
  play_clear_ack(play);
  uint32_t seq = cmd->seq;
  if (xQueueSend(play->cmd_q, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    xSemaphoreGive(play->lock);
    return PLAY_ERR_FAIL;
  }
  TickType_t wait_ticks =
      (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  EventBits_t wanted = (EventBits_t)(ok_bit | fail_bit);
  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, wanted, pdTRUE, pdFALSE, wait_ticks);
  play_ack_t got = play->ack_value;
  uint32_t got_seq = play->ack_seq;
  xSemaphoreGive(play->lock);
  if (bits == 0) {
    return PLAY_ERR_TIMEOUT;
  }
  if (got_seq != seq) {
    return PLAY_ERR_TIMEOUT;
  }
  if ((got & fail_bit) != 0) {
    if (fail_bit == PLAY_ACK_CONFIG_FAIL)
      return PLAY_ERR_CONFIG_FAIL;
    if (fail_bit == PLAY_ACK_PLAY_FAIL)
      return PLAY_ERR_PREBUFFER_TIMEOUT;
    return PLAY_ERR_FAIL;
  }
  if ((got & ok_bit) == 0) {
    return PLAY_ERR_FAIL;
  }
  return PLAY_OK;
}

static void play_drain_to_dac(play_t play) {
  while (buf_get_fill(play->buf_out) > 0) {
    size_t item_size = 0;
    void *item = buf_read_up_to(play->buf_out, &item_size, 4096);
    if (!item || item_size == 0) {
      break;
    }
    size_t off = 0;
    while (off < item_size) {
      size_t written = 0;
      if (i2s_write(play->i2s, (uint8_t *)item + off, item_size - off,
                    &written) != I2S_OK) {
        break;
      }
      if (written == 0) {
        break;
      }
      off += written;
    }
    buf_return_item(play->buf_out, item);
  }
}

static play_err_t play_do_config_locked(play_t play,
                                        const play_src_cfg_t *src_cfg) {
  play_set_state_locked(play, PLAY_STATE_CONFIGURING);
  dec_codec_t dec_codec = play_codec_to_dec(src_cfg->codec);
  if (play->dec == NULL) {
    dec_t d = dec_create(&(dec_cfg_t){.codec = dec_codec});
    if (d == NULL) {
      ESP_LOGE(TAG, "dec_create failed");
      play->diag.err_cnt++;
      play->diag.last_err = PLAY_ERR_DEC_FAIL;
      play_set_state_locked(play, PLAY_STATE_ERROR);
      if (play->cfg.on_error)
        play->cfg.on_error(play->cfg.user_ctx, PLAY_ERR_DEC_FAIL);
      return PLAY_ERR_DEC_FAIL;
    }
    play->dec = d;
  } else {
    if (dec_set_codec(play->dec, dec_codec) != DEC_OK) {
      ESP_LOGE(TAG, "dec_set_codec failed");
      play->diag.err_cnt++;
      play->diag.last_err = PLAY_ERR_DEC_FAIL;
      play_set_state_locked(play, PLAY_STATE_ERROR);
      if (play->cfg.on_error)
        play->cfg.on_error(play->cfg.user_ctx, PLAY_ERR_DEC_FAIL);
      return PLAY_ERR_DEC_FAIL;
    }
  }
  dec_reset(play->dec);
  play->src_cfg = *src_cfg;
  play->configured = true;
  play->need_i2s_cfg = true;
  play->cur_rate = src_cfg->rate;
  play->cur_bits = src_cfg->bits;
  play->cur_ch = src_cfg->channels;
  play->prebuffer_done = false;
  play->src_eof = false;
  play->pending_len = 0;
  buf_flush(play->buf_in);
  buf_flush(play->buf_out);
  if (src_cfg->rate != 0) {
    if (i2s_reconfig(play->i2s, src_cfg->rate, src_cfg->bits,
                     src_cfg->channels) != I2S_OK) {
      play->diag.err_cnt++;
      play->diag.last_err = PLAY_ERR_I2S_FAIL;
      play_set_state_locked(play, PLAY_STATE_ERROR);
      if (play->cfg.on_error)
        play->cfg.on_error(play->cfg.user_ctx, PLAY_ERR_I2S_FAIL);
      return PLAY_ERR_I2S_FAIL;
    }
    play->need_i2s_cfg = false;
    play->diag.reconfig_cnt++;
  }
  return PLAY_OK;
}

static void play_flush_locked(play_t play) {
  play->pending_len = 0;
  play->prebuffer_done = false;
  play->src_eof = false;
  buf_flush(play->buf_in);
  buf_flush(play->buf_out);
  if (play->dec != NULL) {
    dec_reset(play->dec);
  }
}

static void play_task(void *arg) {
  play_t play = (play_t)arg;
  play_cmd_internal_t cmd;
  ESP_LOGI(TAG, "task started");
  while (!play->task_should_exit) {
    TickType_t wait = (play->state == PLAY_STATE_PLAYING)
                          ? pdMS_TO_TICKS(PLAY_DECODE_POLL_MS)
                          : portMAX_DELAY;
    BaseType_t has_cmd = xQueueReceive(play->cmd_q, &cmd, wait);
    if (has_cmd == pdTRUE) {
      switch (cmd.type) {
      case PLAY_CMD_SET_SRC: {
        if (!cmd.has_src_cfg) {
          play_set_ack(play, cmd.seq, PLAY_ACK_CONFIG_FAIL);
          break;
        }
        play->src_cfg = cmd.src_cfg;
        play->src_set = true;
        play_set_ack(play, cmd.seq, PLAY_ACK_CONFIG_OK);
        break;
      }
      case PLAY_CMD_CONFIG: {
        if (!cmd.has_src_cfg || !play->src_set) {
          play_set_state_locked(play, PLAY_STATE_ERROR);
          play_set_ack(play, cmd.seq, PLAY_ACK_CONFIG_FAIL);
          break;
        }
        play_err_t e = play_do_config_locked(play, &cmd.src_cfg);
        if (e == PLAY_OK) {
          play_set_state_locked(play, PLAY_STATE_IDLE);
          play->configured = true;
          play_set_ack(play, cmd.seq, PLAY_ACK_CONFIG_OK);
        } else {
          play_set_ack(play, cmd.seq, PLAY_ACK_CONFIG_FAIL);
        }
        break;
      }
      case PLAY_CMD_PLAY: {
        if (!play->configured || !play->src_set) {
          play_set_ack(play, cmd.seq, PLAY_ACK_PLAY_FAIL);
          break;
        }
        if (play->state == PLAY_STATE_PAUSED) {
          play_set_state_locked(play, PLAY_STATE_PLAYING);
          play_set_ack(play, cmd.seq, PLAY_ACK_RESUME_OK);
          break;
        }
        play_set_state_locked(play, PLAY_STATE_PLAYING);
        play->prebuffer_done = false;
        uint32_t start = xTaskGetTickCount();
        bool pre_ok = false;
        uint8_t enc_tmp[PLAY_ENC_CHUNK];
        while (!play->task_should_exit) {
          if (buf_get_fill(play->buf_in) >= play->cfg.prebuffer_bytes) {
            pre_ok = true;
            break;
          }
          if (play->src_cfg.src_read == NULL)
            break;
          if (buf_get_free(play->buf_in) < PLAY_ENC_CHUNK) {
            pre_ok = true;
            break;
          }
          size_t n = play->src_cfg.src_read(play->src_cfg.src_ctx, enc_tmp,
                                            PLAY_ENC_CHUNK);
          if (n == 0) {
            if (buf_get_fill(play->buf_in) > 0)
              pre_ok = true;
            break;
          }
          if (buf_write(play->buf_in, enc_tmp, n) != BUF_OK)
            break;
          if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(1000))
            break;
        }
        if (!pre_ok) {
          play->diag.err_cnt++;
          play->diag.last_err = PLAY_ERR_PREBUFFER_TIMEOUT;
          play_set_state_locked(play, PLAY_STATE_ERROR);
          play_set_ack(play, cmd.seq, PLAY_ACK_PLAY_FAIL);
        } else {
          play->prebuffer_done = true;
          play_set_ack(play, cmd.seq, PLAY_ACK_PLAY_OK);
        }
        break;
      }
      case PLAY_CMD_PAUSE: {
        if (play->state != PLAY_STATE_PLAYING) {
          play_set_ack(play, cmd.seq, PLAY_ACK_PAUSE_OK);
          break;
        }
        play_set_state_locked(play, PLAY_STATE_PAUSED);
        play_set_ack(play, cmd.seq, PLAY_ACK_PAUSE_OK);
        break;
      }
      case PLAY_CMD_RESUME: {
        if (play->state != PLAY_STATE_PAUSED) {
          play_set_ack(play, cmd.seq, PLAY_ACK_PLAY_FAIL);
          break;
        }
        play_set_state_locked(play, PLAY_STATE_PLAYING);
        play_set_ack(play, cmd.seq, PLAY_ACK_RESUME_OK);
        break;
      }
      case PLAY_CMD_STOP: {
        play_flush_locked(play);
        play_set_state_locked(play, PLAY_STATE_IDLE);
        play_set_ack(play, cmd.seq, PLAY_ACK_STOP_OK);
        break;
      }
      case PLAY_CMD_SHUTDOWN: {
        play_flush_locked(play);
        play_set_state_locked(play, PLAY_STATE_SHUTDOWN);
        play_set_ack(play, cmd.seq, PLAY_ACK_SHUTDOWN_OK);
        play->task_should_exit = true;
        break;
      }
      default:
        break;
      }
      continue;
    }
    if (play->state != PLAY_STATE_PLAYING || !play->prebuffer_done)
      continue;
    if (play->dec == NULL || play->i2s == NULL)
      continue;

    // 1. producent: src -> buf_in, po 4096
    if (!play->src_eof) {
      if (buf_get_free(play->buf_in) >= PLAY_ENC_CHUNK) {
        uint8_t enc_tmp[PLAY_ENC_CHUNK];
        size_t n = play->src_cfg.src_read(play->src_cfg.src_ctx, enc_tmp,
                                          PLAY_ENC_CHUNK);
        if (n == 0) {
          play->src_eof = true;
        } else {
          buf_write(play->buf_in, enc_tmp, n);
        }
      }
    }

    // 2. player: pending + buf_in -> tmp_in -> dec_decode -> pending
    size_t fill_in = buf_get_fill(play->buf_in);
    if (fill_in > 0 || play->pending_len > 0) {
      size_t total_in = play->pending_len;
      memcpy(play->tmp_in, play->pending, play->pending_len);
      if (fill_in > 0) {
        size_t item_size = 0;
        void *item = buf_read_up_to(play->buf_in, &item_size,
                                    PLAY_TMP_IN_SIZE - play->pending_len);
        if (item) {
          memcpy(play->tmp_in + play->pending_len, item, item_size);
          total_in += item_size;
          buf_return_item(play->buf_in, item);
        }
      }
      if (total_in > 0) {
        size_t consumed = 0, produced = 0;
        dec_err_t rc =
            dec_decode(play->dec, play->tmp_in, total_in, play->tmp_pcm,
                       PLAY_TMP_PCM_SIZE, &consumed, &produced);
        if (rc != DEC_OK) {
          ESP_LOGE(TAG, "dec_decode err %d", (int)rc);
          play->diag.err_cnt++;
          play->diag.last_err = PLAY_ERR_DEC_FAIL;
          play_set_state_locked(play, PLAY_STATE_ERROR);
          if (play->cfg.on_error)
            play->cfg.on_error(play->cfg.user_ctx, PLAY_ERR_DEC_FAIL);
          play_set_ack(play, 0, PLAY_ACK_ERROR);
          continue;
        }
        // POPRAWKA: w dec.h nie ma NEED_MORE, need-more to DEC_OK + consumed==0
        // + produced==0 pending = total_in - consumed, to już było dobrze, ale
        // trzeba obsłużyć EOF: jak src EOF i buf_in pusty, a decoder dalej nic
        // nie konsumuje, to znaczy że została niekompletna końcówka, odrzuć ją
        // i leć do EOF, inaczej kręcisz się w kółko i main robi TIMEOUT
        if (consumed < total_in) {
          size_t left = total_in - consumed;
          memmove(play->pending, play->tmp_in + consumed, left);
          play->pending_len = left;
        } else {
          play->pending_len = 0;
        }
        if (consumed == 0 && produced == 0) {
          if (play->src_eof && buf_get_fill(play->buf_in) == 0) {
            ESP_LOGI(TAG, "need-more on EOF, drop pending %u",
                     (unsigned)play->pending_len);
            play->pending_len = 0;
          } else {
            vTaskDelay(pdMS_TO_TICKS(2));
          }
        } else if (produced > 0) {
          dec_info_t info = dec_get_info(play->dec);
          if (info.sample_rate != 0 && info.bits != 0 && info.channels != 0) {
            if (info.sample_rate != play->cur_rate ||
                info.bits != play->cur_bits || info.channels != play->cur_ch ||
                play->need_i2s_cfg) {
              if (i2s_reconfig(play->i2s, info.sample_rate, info.bits,
                               info.channels) == I2S_OK) {
                play->cur_rate = info.sample_rate;
                play->cur_bits = info.bits;
                play->cur_ch = info.channels;
                play->need_i2s_cfg = false;
                play->diag.reconfig_cnt++;
                ESP_LOGI(TAG, "i2s reconfig: %lu Hz %u bit %u ch",
                         (unsigned long)play->cur_rate, play->cur_bits,
                         play->cur_ch);
              }
            }
          }
          // -6dB test, tak jak w starym, bo bylo przesterowanie
          if (play->cur_bits == 24) {
            size_t num_samples = produced / 3;
            for (int i = (int)num_samples - 1; i >= 0; i--) {
              uint8_t *src = play->tmp_pcm + i * 3;
              int32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
              if (v & 0x800000)
                v |= 0xFF000000;
              v = v / 2;
              v = v << 8;
              int32_t *dst = (int32_t *)(play->tmp_pcm + i * 4);
              *dst = v;
            }
            produced = num_samples * 4;
          } else {
            int16_t *pcm = (int16_t *)play->tmp_pcm;
            size_t samples = produced / 2;
            for (size_t i = 0; i < samples; i++) {
              int32_t s = pcm[i];
              s = s / 2;
              pcm[i] = (int16_t)s;
            }
          }
          play->diag.total_frames++;
          play->diag.decoded_bytes += (uint32_t)consumed;
          size_t off = 0;
          while (off < produced) {
            size_t w = buf_write_partial(play->buf_out, play->tmp_pcm + off,
                                         produced - off);
            if (w == 0) {
              play_drain_to_dac(play);
            } else {
              off += w;
            }
          }
          play->diag.played_bytes += (uint32_t)produced;
        }
      }
    }

    // 3. drain buf_out -> i2s
    play_drain_to_dac(play);

    // 4. EOF: src skonczony, buf_in pusty, pending pusty, buf_out pusty
    if (play->src_eof && buf_get_fill(play->buf_in) == 0 &&
        play->pending_len == 0 && buf_get_fill(play->buf_out) == 0) {
      play_drain_to_dac(play);
      play->diag.src_eof_cnt++;
      play_set_state_locked(play, PLAY_STATE_EOF);
      if (play->cfg.on_eof)
        play->cfg.on_eof(play->cfg.user_ctx);
      play_set_ack(play, 0, PLAY_ACK_EOF);
    }
  }
  ESP_LOGI(TAG, "task exiting");
  vTaskDelete(NULL);
}

play_err_t play_create(const play_cfg_t *cfg, play_t *out) {
  if (cfg == NULL || out == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_t play =
      (play_t)heap_caps_malloc(sizeof(struct play_s), MALLOC_CAP_8BIT);
  if (play == NULL)
    return PLAY_ERR_NO_MEM;
  memset(play, 0, sizeof(struct play_s));
  play->cfg = *cfg;
  if (play->cfg.stack_size == 0)
    play->cfg.stack_size = PLAY_DEFAULT_STACK;
  if (play->cfg.priority == 0)
    play->cfg.priority = 5;
  if (play->cfg.buf_in_size == 0)
    play->cfg.buf_in_size = PLAY_DEFAULT_BUF_IN;
  if (play->cfg.buf_out_size == 0)
    play->cfg.buf_out_size = PLAY_DEFAULT_BUF_OUT;
  if (play->cfg.prebuffer_bytes == 0)
    play->cfg.prebuffer_bytes = PLAY_DEFAULT_PREBUFFER;
  play->buf_in = buf_create(play->cfg.buf_in_size);
  play->buf_out = buf_create(play->cfg.buf_out_size);
  play->tmp_in = play_alloc_psram(PLAY_TMP_IN_SIZE);
  play->tmp_pcm = play_alloc_psram(PLAY_TMP_PCM_SIZE);
  play->pending = play_alloc_psram(PLAY_PENDING_SIZE);
  if (play->buf_in == NULL || play->buf_out == NULL || play->tmp_in == NULL ||
      play->tmp_pcm == NULL || play->pending == NULL) {
    if (play->buf_in)
      buf_destroy(play->buf_in);
    if (play->buf_out)
      buf_destroy(play->buf_out);
    if (play->tmp_in)
      heap_caps_free(play->tmp_in);
    if (play->tmp_pcm)
      heap_caps_free(play->tmp_pcm);
    if (play->pending)
      heap_caps_free(play->pending);
    heap_caps_free(play);
    return PLAY_ERR_NO_MEM;
  }
  play->cmd_q = xQueueCreate(PLAY_CMD_QUEUE_LEN, sizeof(play_cmd_internal_t));
  play->ack_group = xEventGroupCreate();
  play->lock = xSemaphoreCreateMutex();
  if (play->cmd_q == NULL || play->ack_group == NULL || play->lock == NULL) {
    if (play->cmd_q)
      vQueueDelete(play->cmd_q);
    if (play->ack_group)
      vEventGroupDelete(play->ack_group);
    if (play->lock)
      vSemaphoreDelete(play->lock);
    buf_destroy(play->buf_in);
    buf_destroy(play->buf_out);
    heap_caps_free(play->tmp_in);
    heap_caps_free(play->tmp_pcm);
    heap_caps_free(play->pending);
    heap_caps_free(play);
    return PLAY_ERR_NO_MEM;
  }
  play->state = PLAY_STATE_IDLE;
  *out = play;
  return PLAY_OK;
}

play_err_t play_destroy(play_t play) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  if (play->task_handle != NULL)
    return PLAY_ERR_ALREADY_STARTED;
  if (play->dec != NULL)
    dec_destroy(play->dec);
  if (play->i2s != NULL) {
    i2s_disable(play->i2s);
    i2s_destroy(play->i2s);
  }
  if (play->buf_in)
    buf_destroy(play->buf_in);
  if (play->buf_out)
    buf_destroy(play->buf_out);
  if (play->cmd_q)
    vQueueDelete(play->cmd_q);
  if (play->ack_group)
    vEventGroupDelete(play->ack_group);
  if (play->lock)
    vSemaphoreDelete(play->lock);
  if (play->tmp_in)
    heap_caps_free(play->tmp_in);
  if (play->tmp_pcm)
    heap_caps_free(play->tmp_pcm);
  if (play->pending)
    heap_caps_free(play->pending);
  heap_caps_free(play);
  return PLAY_OK;
}

play_err_t play_start(play_t play) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  if (play->task_handle != NULL)
    return PLAY_ERR_ALREADY_STARTED;
  dec_register_default();
  i2s_t dac = i2s_create(&play->cfg.i2s_cfg);
  if (dac == NULL)
    return PLAY_ERR_I2S_FAIL;
  i2s_enable(dac);
  play->i2s = dac;
  play->task_should_exit = false;
  BaseType_t ok = xTaskCreatePinnedToCore(
      play_task, "play", play->cfg.stack_size, play, play->cfg.priority,
      &play->task_handle, play->cfg.core_id);
  if (ok != pdPASS) {
    i2s_destroy(play->i2s);
    play->i2s = NULL;
    return PLAY_ERR_NO_MEM;
  }
  return PLAY_OK;
}

play_err_t play_set_src(play_t play, const play_src_cfg_t *src_cfg,
                        uint32_t timeout_ms) {
  if (play == NULL || src_cfg == NULL || src_cfg->src_read == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_SET_SRC;
  cmd.seq = ++play->seq;
  cmd.src_cfg = *src_cfg;
  cmd.has_src_cfg = true;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_CONFIG_OK,
                                PLAY_ACK_CONFIG_FAIL, timeout_ms);
}

play_err_t play_config(play_t play, const play_src_cfg_t *src_cfg,
                       uint32_t timeout_ms) {
  if (play == NULL || src_cfg == NULL || src_cfg->src_read == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_CONFIG;
  cmd.seq = ++play->seq;
  cmd.src_cfg = *src_cfg;
  cmd.has_src_cfg = true;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_CONFIG_OK,
                                PLAY_ACK_CONFIG_FAIL, timeout_ms);
}

play_err_t play_play(play_t play, uint32_t timeout_ms) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_PLAY;
  cmd.seq = ++play->seq;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_PLAY_OK,
                                PLAY_ACK_PLAY_FAIL, timeout_ms);
}

play_err_t play_pause(play_t play, uint32_t timeout_ms) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_PAUSE;
  cmd.seq = ++play->seq;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_PAUSE_OK, PLAY_ACK_ERROR,
                                timeout_ms);
}

play_err_t play_resume(play_t play, uint32_t timeout_ms) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_RESUME;
  cmd.seq = ++play->seq;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_RESUME_OK,
                                PLAY_ACK_PLAY_FAIL, timeout_ms);
}

play_err_t play_stop(play_t play, uint32_t timeout_ms) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_STOP;
  cmd.seq = ++play->seq;
  return play_send_cmd_and_wait(play, &cmd, PLAY_ACK_STOP_OK, PLAY_ACK_ERROR,
                                timeout_ms);
}

play_err_t play_shutdown(play_t play, uint32_t timeout_ms) {
  if (play == NULL)
    return PLAY_ERR_INVALID_ARG;
  if (play->task_handle == NULL)
    return PLAY_ERR_NOT_STARTED;
  play_cmd_internal_t cmd = {0};
  cmd.type = PLAY_CMD_SHUTDOWN;
  cmd.seq = ++play->seq;
  play_err_t e = play_send_cmd_and_wait(play, &cmd, PLAY_ACK_SHUTDOWN_OK,
                                        PLAY_ACK_ERROR, timeout_ms);
  for (int i = 0; i < 50 && play->task_handle != NULL; i++) {
    vTaskDelay(pdMS_TO_TICKS(10));
    if (play->task_should_exit)
      break;
  }
  play->task_handle = NULL;
  return e;
}

play_err_t play_get_state(play_t play, play_state_t *state) {
  if (play == NULL || state == NULL)
    return PLAY_ERR_INVALID_ARG;
  *state = play->state;
  return PLAY_OK;
}

play_err_t play_get_diag(play_t play, play_diag_t *diag) {
  if (play == NULL || diag == NULL)
    return PLAY_ERR_INVALID_ARG;
  *diag = play->diag;
  return PLAY_OK;
}

const char *play_err_to_str(play_err_t err) {
  switch (err) {
  case PLAY_OK:
    return "OK";
  case PLAY_ERR_INVALID_ARG:
    return "INVALID_ARG";
  case PLAY_ERR_FAIL:
    return "FAIL";
  case PLAY_ERR_NO_MEM:
    return "NO_MEM";
  case PLAY_ERR_ALREADY_STARTED:
    return "ALREADY_STARTED";
  case PLAY_ERR_NOT_STARTED:
    return "NOT_STARTED";
  case PLAY_ERR_TIMEOUT:
    return "TIMEOUT";
  case PLAY_ERR_CONFIG_FAIL:
    return "CONFIG_FAIL";
  case PLAY_ERR_DEC_FAIL:
    return "DEC_FAIL";
  case PLAY_ERR_I2S_FAIL:
    return "I2S_FAIL";
  case PLAY_ERR_SRC_FAIL:
    return "SRC_FAIL";
  case PLAY_ERR_INVALID_STATE:
    return "INVALID_STATE";
  case PLAY_ERR_PREBUFFER_TIMEOUT:
    return "PREBUFFER_TIMEOUT";
  default:
    return "UNKNOWN";
  }
}