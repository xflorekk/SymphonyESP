/**
 * @file main.c
 * @brief Loop nieskończony: mp3 -> aac -> pcm -> mp3...
 * Każdy utwór: PLAY, po 2s PAUSE na 2s, RESUME, dograj do EOF.
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
extern const uint8_t test_5s_aac_start[] asm("_binary_test_5s_aac_start");
extern const uint8_t test_5s_aac_end[] asm("_binary_test_5s_aac_end");
extern const uint8_t test_5s_pcm_start[] asm("_binary_test_5s_pcm_start");
extern const uint8_t test_5s_pcm_end[] asm("_binary_test_5s_pcm_end");

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

static void log_diag(player_t play, const char *tag) {
  player_diag_t d;
  if (player_get_diag(play, &d) == PLAYER_OK) {
    ESP_LOGI(TAG,
             "[MAIN DIAG %s] state=%d frames=%u pcm=%u i2s=%u heap=%u min=%u "
             "stack_hwm=%u",
             tag, (int)d.state, (unsigned)d.frames, (unsigned)d.pcm_bytes,
             (unsigned)d.i2s_bytes, (unsigned)d.heap_free, (unsigned)d.heap_min,
             (unsigned)d.stack_hwm);
  }
}

// Jeden utwór: CONFIG -> PLAY -> feed z pauzą 2s po 2s grania -> EOF
static void play_track(player_t play, buf_t buf_in, const char *name,
                       const uint8_t *data, size_t len, player_dec_cfg_t cfg) {
  ESP_LOGI(TAG, "===== TRACK %s START len=%u =====", name, (unsigned)len);
  CHECK(player_cmd_config(play, &cfg, 1000), "CONFIG");
  CHECK(player_cmd_play(play, 1000), "PLAY");

  TickType_t start = xTaskGetTickCount();
  bool paused_once = false;
  size_t pos = 0;

  while (pos < len) {
    // Pauza po 2s grania, w trakcie feedu
    if (!paused_once && (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(2000)) {
      ESP_LOGI(TAG, "[%s] 2s grania, PAUSE na 2s", name);
      CHECK(player_cmd_pause(play, 1000), "PAUSE 2s");
      vTaskDelay(pdMS_TO_TICKS(2000));
      ESP_LOGI(TAG, "[%s] RESUME", name);
      CHECK(player_cmd_resume(play, 1000), "RESUME");
      paused_once = true;
    }
    // Feed z naturalnym backpressure, bez buf_get_free i bez delay 5ms
    size_t want = len - pos > 1024 ? 1024 : len - pos;
    size_t w = buf_write_partial(buf_in, data + pos, want);
    if (w == 0) {
      // bufor pełny, player jeszcze nie zjadł, czekaj
      vTaskDelay(pdMS_TO_TICKS(2));
    } else {
      pos += w;
      // brak delay, tempo dyktuje player
    }
  }

  // Feed skończył się przed 2s, np. MP3 w 1s, a pauza ma być po 2s grania
  // Czekaj do 2s od PLAY i dopiero pauzuj
  if (!paused_once) {
    TickType_t elapsed = xTaskGetTickCount() - start;
    if (elapsed < pdMS_TO_TICKS(2000)) {
      vTaskDelay(pdMS_TO_TICKS(2000) - elapsed);
    }
    ESP_LOGI(TAG, "[%s] 2s grania po feedzie, PAUSE na 2s", name);
    CHECK(player_cmd_pause(play, 1000), "PAUSE 2s");
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "[%s] RESUME", name);
    CHECK(player_cmd_resume(play, 1000), "RESUME");
    paused_once = true;
  }

  ESP_LOGI(TAG, "[%s] feed done, EOF", name);
  CHECK(player_cmd_eof(play, 8000), "EOF");
  vTaskDelay(pdMS_TO_TICKS(300));
  log_diag(play, name);
  ESP_LOGI(TAG, "===== TRACK %s DONE =====", name);
  vTaskDelay(pdMS_TO_TICKS(500));
}

void app_main(void) {
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
  size_t aac_len = test_5s_aac_end - test_5s_aac_start;
  size_t pcm_len = test_5s_pcm_end - test_5s_pcm_start;
  ESP_LOGI(TAG, "mp3 len %u aac len %u pcm len %u expected PCM ~882000",
           (unsigned)mp3_len, (unsigned)aac_len, (unsigned)pcm_len);

  player_dec_cfg_t cfg_mp3 = {
      .codec = DEC_CODEC_MP3, .sample_rate = 44100, .bits = 16, .channels = 2};
  player_dec_cfg_t cfg_aac = {
      .codec = DEC_CODEC_AAC, .sample_rate = 44100, .bits = 16, .channels = 2};
  // PCM: CONFIG musi nieść rate/bits/ch, bo PCM nie ma nagłówka
  player_dec_cfg_t cfg_pcm = {
      .codec = DEC_CODEC_PCM, .sample_rate = 44100, .bits = 16, .channels = 2};

  int iter = 0;
  for (;;) {
    ESP_LOGI(TAG, "===== LOOP %d START =====", iter++);
    play_track(play, buf_in, "MP3", test_5s_mp3_start, mp3_len, cfg_mp3);
    play_track(play, buf_in, "AAC", test_5s_aac_start, aac_len, cfg_aac);
    play_track(play, buf_in, "PCM", test_5s_pcm_start, pcm_len, cfg_pcm);
    ESP_LOGI(TAG, "===== LOOP %d DONE =====", iter - 1);
  }
}