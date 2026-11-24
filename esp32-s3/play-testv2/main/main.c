/**
 * @file main.c
 * @brief Test player po poprawkach przejść: UNCONFIGURED zeruje config.
 *
 * Src (main) jest odpowiedzialny za kolejność komend:
 *  ITER 1/2: CONFIG (z UNCONFIGURED) -> PLAY -> feed -> EOF -> UNCONFIGURED
 *  ITER 3:   CONFIG -> PLAY -> feed 30k -> STOP -> IDLE
 *            -> CONFIG = FAIL (oczekiwane, tylko z UNCONFIGURED)
 *            -> RESET -> UNCONFIGURED -> CONFIG -> PLAY -> feed full -> EOF
 */
#include "buf.h"
#include "dec.h"
#include "i2s.h"
#include "player.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "main";
static player_storage_t storage;

extern const uint8_t test_5s_mp3_start[] asm("_binary_test_5s_mp3_start");
extern const uint8_t test_5s_mp3_end[] asm("_binary_test_5s_mp3_end");

static void on_state(void *ctx, player_state_t state) {
  const char *n = "?";
  switch (state) {
  case PLAYER_STATE_IDLE:
    n = "IDLE";
    break;
  case PLAYER_STATE_UNCONFIGURED:
    n = "UNCONFIGURED";
    break;
  case PLAYER_STATE_CONFIGURING:
    n = "CONFIGURING";
    break;
  case PLAYER_STATE_PLAYING:
    n = "PLAYING";
    break;
  case PLAYER_STATE_PAUSED:
    n = "PAUSED";
    break;
  }
  ESP_LOGI(TAG, "on_state: %s", n);
}
static void on_error(void *ctx, player_err_t err) {
  ESP_LOGE(TAG, "on_error: %s", player_err_to_str(err));
}

static void log_player_diag(player_t play, const char *step) {
  player_diag_t d = {0};
  player_get_diag(play, &d);
  ESP_LOGI(TAG, "%s: state=%d heap=%u heap_min=%u stack_hwm=%lu", step, d.state,
           (unsigned)d.heap_free, (unsigned)d.heap_min,
           (unsigned long)d.stack_hwm);
}
static void log_hw_diag(i2s_t dac, dec_t dec, buf_t buf_in, buf_t buf_out,
                        const char *step) {
  i2s_diag_t di = i2s_get_diag(dac);
  dec_diag_t dd = dec_get_diag(dec);
  buf_diag_t bi = buf_get_diag(buf_in);
  buf_diag_t bo = buf_get_diag(buf_out);
  ESP_LOGI(TAG,
           "%s i2s: bytes=%lu reconfig=%lu | dec: frames=%lu "
           "bytes_out=%lu | buf_in max=%lu buf_out max=%lu",
           step, (unsigned long)di.write_bytes, /*(unsigned long)di.err_cnt,*/
           (unsigned long)di.reconfig_cnt, (unsigned long)dd.frame_cnt,
           (unsigned long)dd.bytes_out, (unsigned long)bi.max_fill,
           (unsigned long)bo.max_fill);
}

#define CHECK(x, msg)                                                          \
  do {                                                                         \
    player_err_t _r = (x);                                                     \
    ESP_LOGI(TAG, "%s -> %s", msg, player_err_to_str(_r));                     \
    if (_r != PLAYER_OK) {                                                     \
      ESP_LOGE(TAG, "FAIL at %s", msg);                                        \
      for (;;)                                                                 \
        vTaskDelay(pdMS_TO_TICKS(1000));                                       \
    }                                                                          \
  } while (0)

/**
 * @brief Src: karmi buf_in całym plikiem, oddaje CPU po każdym chunku.
 * @details Po każdym udanym buf_write vTaskDelay(5), żeby player na CPU1
 *          miał czas na decode i IDLE0 nie głodował. Przy FAIL / pełnym
 *          buf vTaskDelay(10). Log co 10240 bajtów.
 */
static void feed_mp3(buf_t buf_in, size_t mp3_len) {
  size_t pos = 0;
  uint32_t last_log = 0;
  while (pos < mp3_len) {
    size_t free_in = buf_get_free(buf_in);
    if (free_in >= 2048) {
      size_t want = mp3_len - pos > 1024 ? 1024 : mp3_len - pos;
      if (buf_write(buf_in, test_5s_mp3_start + pos, want) == BUF_OK) {
        pos += want;
        vTaskDelay(pdMS_TO_TICKS(5));
        if (pos - last_log >= 10240) {
          ESP_LOGI(TAG, "src pos %u / %u free %u", (unsigned)pos,
                   (unsigned)mp3_len, (unsigned)free_in);
          last_log = pos;
        }
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  ESP_LOGI(TAG, "src done pos %u / %u", (unsigned)pos, (unsigned)mp3_len);
}

static void feed_mp3_partial(buf_t buf_in, size_t bytes) {
  size_t pos = 0;
  while (pos < bytes) {
    if (buf_get_free(buf_in) >= 2048) {
      size_t want = bytes - pos > 1024 ? 1024 : bytes - pos;
      if (buf_write(buf_in, test_5s_mp3_start + pos, want) == BUF_OK) {
        pos += want;
        vTaskDelay(pdMS_TO_TICKS(5));
      } else {
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
  ESP_LOGI(TAG, "src partial done %u", (unsigned)pos);
}

void app_main(void) {
  ESP_LOGI(TAG, "heap before hw: %u", (unsigned)xPortGetFreeHeapSize());

  buf_t buf_in = buf_create(32 * 1024);
  buf_t buf_out = buf_create(32 * 1024);
  i2s_cfg_t i2s_cfg = {
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
  i2s_t dac = i2s_create(&i2s_cfg);
  dec_register_default();
  dec_t dec = dec_create(&(dec_cfg_t){.codec = DEC_CODEC_MP3});
  if (!buf_in || !buf_out || !dac || !dec) {
    ESP_LOGE(TAG, "hw create fail");
    for (;;)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }
  ESP_LOGI(TAG, "heap after hw: %u", (unsigned)xPortGetFreeHeapSize());

  player_hw_t hw = {
      .i2s = dac, .buf_in = buf_in, .buf_out = buf_out, .dec = dec};
  player_cfg_t pcfg = {.on_state = on_state,
                       .on_error = on_error,
                       .user_ctx = NULL,
                       .priority = 5,
                       .core_id = 1};
  player_t play = NULL;
  CHECK(player_init(&storage, &pcfg, &hw, &play), "player_init");
  CHECK(player_start(play), "player_start");
  vTaskDelay(pdMS_TO_TICKS(200));

  size_t mp3_len = test_5s_mp3_end - test_5s_mp3_start;
  ESP_LOGI(TAG, "mp3 len %u", (unsigned)mp3_len);
  player_dec_cfg_t mp3 = {
      .codec = DEC_CODEC_MP3, .sample_rate = 44100, .bits = 16, .channels = 2};

  size_t heap_eof1 = 0, heap_eof2 = 0, heap_eof3 = 0;

  // ===== ITER 1 + ITER 2: EOF -> UNCONFIGURED -> CONFIG =====
  for (int iter = 0; iter < 2; iter++) {
    ESP_LOGI(TAG, "===== ITER %d (EOF path) =====", iter + 1);
    CHECK(player_cmd_config(play, &mp3, 1000), "CONFIG MP3");
    log_player_diag(play, "after CONFIG");
    CHECK(player_cmd_play(play, 1000), "PLAY");

    feed_mp3(buf_in, mp3_len);

    CHECK(player_cmd_eof(play, 5000), "EOF");
    vTaskDelay(pdMS_TO_TICKS(200));
    log_player_diag(play, "after EOF");
    log_hw_diag(dac, dec, buf_in, buf_out, "after EOF");

    player_diag_t pd = {0};
    player_get_diag(play, &pd);
    if (iter == 0)
      heap_eof1 = pd.heap_free;
    else
      heap_eof2 = pd.heap_free;

    // diag reset na następną iterację
    i2s_reset_diag(dac);
    buf_reset_diag(buf_in);
    buf_reset_diag(buf_out);
    dec_reset_diag(dec);
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // ===== ITER 3: STOP -> IDLE -> CONFIG musi FAIL, potem RESET -> CONFIG =====
  ESP_LOGI(TAG, "===== ITER 3 (STOP/RESET path) =====");
  CHECK(player_cmd_config(play, &mp3, 1000), "CONFIG MP3 (iter3)");
  CHECK(player_cmd_play(play, 1000), "PLAY (iter3)");
  feed_mp3_partial(buf_in, 30720);
  vTaskDelay(pdMS_TO_TICKS(200));
  CHECK(player_cmd_stop(play, 1000), "STOP -> IDLE");
  log_player_diag(play, "after STOP (expect IDLE)");

  // CONFIG z IDLE ma się nie udać - tylko z UNCONFIGURED
  player_err_t rc = player_cmd_config(play, &mp3, 1000);
  ESP_LOGI(TAG, "CONFIG from IDLE -> %s (expect FAIL)", player_err_to_str(rc));
  if (rc == PLAYER_OK) {
    ESP_LOGE(TAG,
             "BUG: CONFIG from IDLE should FAIL, check player_task validation");
    for (;;)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  CHECK(player_cmd_reset(play, 1000), "RESET -> UNCONFIGURED");
  log_player_diag(play, "after RESET (expect UNCONFIGURED)");

  // Teraz CONFIG legalny
  CHECK(player_cmd_config(play, &mp3, 1000), "CONFIG MP3 after RESET");
  CHECK(player_cmd_play(play, 1000), "PLAY after RESET");
  feed_mp3(buf_in, mp3_len);
  CHECK(player_cmd_eof(play, 5000), "EOF (iter3)");
  vTaskDelay(pdMS_TO_TICKS(200));
  log_player_diag(play, "after EOF iter3");
  log_hw_diag(dac, dec, buf_in, buf_out, "after EOF iter3");
  {
    player_diag_t pd = {0};
    player_get_diag(play, &pd);
    heap_eof3 = pd.heap_free;
  }

  // ===== Werdykt =====
  ESP_LOGI(TAG, "heap_eof1=%u heap_eof2=%u heap_eof3=%u", (unsigned)heap_eof1,
           (unsigned)heap_eof2, (unsigned)heap_eof3);
  if (heap_eof1 == heap_eof2 && heap_eof2 == heap_eof3) {
    ESP_LOGI(TAG, "HEAP OK - no leak, UNCONFIGURED zeruje config");
  } else {
    ESP_LOGE(TAG, "HEAP LEAK - heap spada po każdym EOF");
  }

  CHECK(player_cmd_exit(play, 1000), "EXIT");
  player_destroy(play);
  buf_destroy(buf_in);
  buf_destroy(buf_out);
  dec_destroy(dec);
  i2s_destroy(dac);
  ESP_LOGI(TAG, "TEST OK");
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
}