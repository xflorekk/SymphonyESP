/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Maciek Pollok
 */

/**
 * @file main.c
 * @brief Test play: MP3 <-> AAC <-> FLAC na zmiane, EOF + 1s przerwy.
 *
 * Nowy test na play.h / play.c, inicjalizacja 1:1 ze starym main.c
 * do testowania dec / i2s / buf.
 */

#include "play.h"

#include "i2s.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

static const char *TAG = "main_test";

// EMBED_FILES w CMakeLists.txt:
// EMBED_FILES "test_5s.mp3" "test_5s.aac" "test_5s.flac"
extern const uint8_t test_5s_mp3_start[] asm("_binary_test_5s_mp3_start");
extern const uint8_t test_5s_mp3_end[] asm("_binary_test_5s_mp3_end");
extern const uint8_t test_5s_aac_start[] asm("_binary_test_5s_aac_start");
extern const uint8_t test_5s_aac_end[] asm("_binary_test_5s_aac_end");
extern const uint8_t test_5s_flac_start[] asm("_binary_test_5s_flac_start");
extern const uint8_t test_5s_flac_end[] asm("_binary_test_5s_flac_end");

/**
 * @brief Mem src, 1 src = 1 plik w flash.
 */
typedef struct {
  const uint8_t *data;
  size_t len;
  size_t pos;
  const char *name;
} mem_src_t;

static size_t mem_src_read(void *ctx, uint8_t *buf, size_t len) {
  mem_src_t *s = (mem_src_t *)ctx;
  if (s == NULL || buf == NULL) {
    return 0;
  }
  size_t remain = s->len - s->pos;
  if (remain == 0) {
    return 0; // EOF
  }
  size_t n = remain < len ? remain : len;
  memcpy(buf, s->data + s->pos, n);
  s->pos += n;
  return n;
}

static bool mem_src_seek(void *ctx, size_t pos) {
  mem_src_t *s = (mem_src_t *)ctx;
  if (s == NULL || pos > s->len) {
    return false;
  }
  s->pos = pos;
  return true;
}

static void on_state_cb(void *user_ctx, play_state_t state) {
  const char *names[] = {"IDLE", "CONFIGURING", "PLAYING", "PAUSED",
                         "EOF",  "ERROR",       "SHUTDOWN"};
  const char *n = "UNKNOWN";
  if (state >= 0 && state <= PLAY_STATE_SHUTDOWN) {
    n = names[state];
  }
  ESP_LOGI(TAG, "on_state: %s", n);
  (void)user_ctx;
}

static void on_eof_cb(void *user_ctx) {
  ESP_LOGI(TAG, "on_eof: file ended");
  (void)user_ctx;
}

static void on_error_cb(void *user_ctx, play_err_t err) {
  ESP_LOGE(TAG, "on_error: %s (%d)", play_err_to_str(err), (int)err);
  (void)user_ctx;
}

/**
 * @brief Graj jeden plik, czekaj na EOF, potem STOP.
 * @return true jak EOF przyszedl, false jak timeout / blad
 */
static bool play_one(play_t play, mem_src_t *src, play_codec_t codec,
                     uint8_t bits, uint32_t rate) {
  ESP_LOGI(TAG, "=== PLAY %s, codec=%d, bits=%u, len=%u ===", src->name,
           (int)codec, bits, (unsigned)src->len);
  src->pos = 0;

  play_src_cfg_t src_cfg = {0};
  src_cfg.codec = codec;
  src_cfg.bits = bits;
  src_cfg.channels = 2;
  src_cfg.rate = rate;
  src_cfg.src_read = mem_src_read;
  src_cfg.src_seek = mem_src_seek;
  src_cfg.src_ctx = src;

  play_err_t e = play_set_src(play, &src_cfg, 1000);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_set_src %s failed: %s", src->name, play_err_to_str(e));
    return false;
  }

  e = play_config(play, &src_cfg, 1000);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_config %s failed: %s", src->name, play_err_to_str(e));
    return false;
  }

  e = play_play(play, 2000);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_play %s failed: %s", src->name, play_err_to_str(e));
    return false;
  }

  // czekamy na EOF, max 10s, plik ma 5s
  for (int i = 0; i < 100; i++) {
    play_state_t st = PLAY_STATE_IDLE;
    play_get_state(play, &st);
    if (st == PLAY_STATE_EOF) {
      ESP_LOGI(TAG, "EOF detected for %s after %d x 100ms", src->name, i);
      break;
    }
    if (st == PLAY_STATE_ERROR) {
      ESP_LOGE(TAG, "ERROR state during %s", src->name);
      play_diag_t d = {0};
      play_get_diag(play, &d);
      ESP_LOGE(
          TAG,
          "diag: frames=%u decoded=%u played=%u under=%u reconfig=%u err=%s",
          (unsigned)d.total_frames, (unsigned)d.decoded_bytes,
          (unsigned)d.played_bytes, (unsigned)d.underruns,
          (unsigned)d.reconfig_cnt, play_err_to_str(d.last_err));
      return false;
    }
    if (i == 99) {
      ESP_LOGE(TAG, "TIMEOUT: no EOF for %s in 10s", src->name);
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  // po EOF zawsze STOP, zeby wrocic do IDLE przed nastepnym CONFIG
  e = play_stop(play, 1000);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_stop %s failed: %s", src->name, play_err_to_str(e));
    return false;
  }

  play_diag_t diag = {0};
  play_get_diag(play, &diag);
  ESP_LOGI(TAG,
           "diag %s: frames=%u decoded=%u played=%u under=%u over=%u "
           "reconfig=%u eof=%u err_cnt=%u last=%s",
           src->name, (unsigned)diag.total_frames, (unsigned)diag.decoded_bytes,
           (unsigned)diag.played_bytes, (unsigned)diag.underruns,
           (unsigned)diag.overruns, (unsigned)diag.reconfig_cnt,
           (unsigned)diag.src_eof_cnt, (unsigned)diag.err_cnt,
           play_err_to_str(diag.last_err));
  return true;
}

void app_main(void) {
  ESP_LOGI(TAG, "test start: MP3 <-> AAC <-> FLAC, EOF + 1s gap");

  mem_src_t mp3_src = {
      .data = test_5s_mp3_start,
      .len = (size_t)(test_5s_mp3_end - test_5s_mp3_start),
      .pos = 0,
      .name = "test_5s.mp3",
  };
  mem_src_t aac_src = {
      .data = test_5s_aac_start,
      .len = (size_t)(test_5s_aac_end - test_5s_aac_start),
      .pos = 0,
      .name = "test_5s.aac",
  };
  mem_src_t flac_src = {
      .data = test_5s_flac_start,
      .len = (size_t)(test_5s_flac_end - test_5s_flac_start),
      .pos = 0,
      .name = "test_5s.flac",
  };
  ESP_LOGI(TAG, "mp3 len=%u, aac len=%u, flac len=%u", (unsigned)mp3_src.len,
           (unsigned)aac_src.len, (unsigned)flac_src.len);

  // tak jak w starym main.c: najpierw rejestrujemy kodeki
  // dec_register_default();

  play_cfg_t cfg = {0};
  cfg.on_state = on_state_cb;
  cfg.on_eof = on_eof_cb;
  cfg.on_error = on_error_cb;
  cfg.user_ctx = NULL;
  cfg.stack_size = 16384;
  cfg.priority = 5;
  cfg.core_id = 0;
  cfg.buf_in_size = 32768;
  cfg.buf_out_size = 65536;
  cfg.prebuffer_bytes = 8192;

  // --- POPRAWKA: i2s_cfg 1:1 ze starego main.c, tego brakowalo w nowym ---
  cfg.i2s_cfg = (i2s_cfg_t){
      .port = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .is_tx = true,
      .use_apll = true,
      .mclk_gpio = 4,
      .bclk_gpio = 5,
      .ws_gpio = 6,
      .dout_gpio = 7,
      .din_gpio = -1,
      .rate = 44100,
      .bits = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_mode = I2S_SLOT_MODE_STEREO,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
  };

  play_t play = NULL;
  play_err_t e = play_create(&cfg, &play);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_create failed: %s", play_err_to_str(e));
    return;
  }

  // play_start() w play.c musi teraz zrobic:
  // i2s_create(&cfg.i2s_cfg) + i2s_enable()
  e = play_start(play);
  if (e != PLAY_OK) {
    ESP_LOGE(TAG, "play_start failed: %s", play_err_to_str(e));
    play_destroy(play);
    return;
  }

  while (1) {
    ESP_LOGI(TAG, "--- LOOP ---");

    bool ok1 = play_one(play, &mp3_src, PLAY_CODEC_MP3, 16, 44100);
    ESP_LOGI(TAG, "MP3 result: %s, pause 1s", ok1 ? "OK" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool ok2 = play_one(play, &aac_src, PLAY_CODEC_AAC, 16, 44100);
    ESP_LOGI(TAG, "AAC result: %s, pause 1s", ok2 ? "OK" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(1000));

    // FLAC: w starym tescie byl 16-bit, jak masz 24-bit plik to daj bits=24
    // play.c i tak po 1. ramce zrobi i2s_reconfig z dec_get_info()
    bool ok3 = play_one(play, &flac_src, PLAY_CODEC_FLAC, 24, 44100);
    ESP_LOGI(TAG, "FLAC result: %s, pause 1s", ok3 ? "OK" : "FAIL");
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!ok1 || !ok2 || !ok3) {
      ESP_LOGE(TAG, "loop FAILED, stop test, check diag");
      break;
    }
  }

  ESP_LOGI(TAG, "test done, shutdown");
  play_shutdown(play, 1000);
  play_destroy(play);
}