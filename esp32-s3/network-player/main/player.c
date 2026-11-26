#include "player.h"
#include "app_sync.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "PLAYER";

typedef struct {
  bool target_playing;
  bool need_config;
  bool need_shutdown;
  uint32_t rate;
  uint8_t channels;
  uint8_t bits;
  buf_t buf_in;
  buf_t buf_out;
  dec_codec_t codec;
  dec_t dec;
  i2s_t dac;
} player_ctx_t;

typedef enum {
  PLAYER_IDLE = 0,
  PLAYER_CONFIGURING,
  PLAYER_BUFFERING,
  PLAYER_PLAYING,
  PLAYER_SHUTDOWN,
} player_state_t;

static player_ctx_t s_ctx;
static player_state_t s_state;
static QueueHandle_t s_queue = NULL;

static void player_decode_step(void) {
  size_t sz = 0;
  void *enc = buf_read_up_to(s_ctx.buf_in, &sz, 4096);
  if (!enc)
    return;
  static int16_t pcm[4096]; // na stosie tasku, nie w s_ctx!
  size_t consumed = 0, produced = 0;
  if (dec_decode(s_ctx.dec, enc, sz, pcm, sizeof(pcm), &consumed, &produced) ==
      0) {
    if (produced > 0)
      buf_write(s_ctx.buf_out, (uint8_t *)pcm, produced);
  }
  buf_return_item(s_ctx.buf_in, enc);
  // jak consumed == 0 -> niepełna ramka, czekaj na więcej danych
}

static void player_play_step(void) {
  size_t sz = 0;
  void *pcm = buf_read_up_to(s_ctx.buf_out, &sz, 4096);
  if (!pcm)
    return;
  size_t written = 0;
  i2s_write(s_ctx.dac, pcm, sz, &written);
  buf_return_item(s_ctx.buf_out, pcm);
}

static void player_task(void *pvParameters) {
  player_cmd_t cmd;

  while (1) {

    TickType_t queue_wait = (s_state == PLAYER_IDLE) ? portMAX_DELAY : 0;
    if (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {

      switch (cmd.type) {
      case PLAYER_CMD_OK:
        break;

      case PLAYER_CMD_PLAY:
        ESP_LOGI(TAG, "play!");
        s_ctx.target_playing = true;
        break;

      case PLAYER_CMD_STOP:
        ESP_LOGI(TAG, "stop!");
        s_ctx.target_playing = false;
        break;

      case PLAYER_CMD_CONFIG:
        ESP_LOGI(TAG, "config!");
        s_ctx.codec = cmd.codec;
        s_ctx.rate = cmd.rate;
        s_ctx.channels = cmd.channels;
        s_ctx.bits = cmd.bits;
        s_ctx.need_config = true;
        break;

      case PLAYER_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_ctx.need_shutdown = true;
        break;

      default:
        break;
      }
    }

    if (s_ctx.need_shutdown && s_state != PLAYER_SHUTDOWN) {
      s_state = PLAYER_SHUTDOWN;
    }

    switch (s_state) {

    case PLAYER_IDLE:
      if (s_ctx.need_config) {
        s_state = PLAYER_CONFIGURING;
      } else if (ctx.target_playing) {
        s_state = PLAYER_BUFFERING;
      }
      break;

    case PLAYER_CONFIGURING:
      ESP_LOGI(TAG, "configuring! codec=%d rate=%lu", s_ctx.codec, s_ctx.rate);

      buf_flush(s_ctx.buf_in);
      buf_flush(s_ctx.buf_out);
      dec_set_codec(s_ctx.dec, s_ctx.codec);
      i2s_reconfig_clk(s_ctx.dac, s_ctx.rate);
      s_ctx.need_config = false;
      s_state = PLAYER_IDLE;
      xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_READY);
      break;

    case PLAYER_BUFFERING:
      if (!s_ctx.target_playing) {
        s_state = PLAYER_IDLE;
        break;
      }
      if (buf_get_fill(s_ctx.buf_out) > 8192) { // mamy 45ms PCM
        s_state = PLAYER_PLAYING;
        xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_PLAYING);
        break;
      }
      // dekoduj aż buf_out się zapełni albo buf_in się skończy
      player_decode_step();
      if (buf_get_fill(s_ctx.buf_in) == 0)
        vTaskDelay(pdMS_TO_TICKS(5));
      break;

    case PLAYER_PLAYING:
      if (!s_ctx.target_playing) {
        s_state = PLAYER_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_STOPPED);
        break;
      }
      if (buf_get_fill(s_ctx.buf_out) < 2048) { // underrun -> buforuj
        s_state = PLAYER_BUFFERING;
        break;
      }
      // dekoduj na zapas + graj
      player_decode_step();
      player_play_step();
      break;

    case PLAYER_SHUTDOWN:
      buf_flush(s_ctx.buf_in);
      buf_flush(s_ctx.buf_out);
      dec_reset(s_ctx.dec);
      i2s_disable(s_ctx.dac);
      s_ctx.need_config = true;
      s_ctx.target_playing = false;
      s_ctx.need_shutdown = false;
      s_state = PLAYER_IDLE;
      xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_STOPPED);
      break;

    default:
      break;
    }
  }
}

int player_init(const player_cfg_t *cfg) {
  if (!cfg) {
    return -1;
  }
  memset(&s_ctx, 0, sizeof(s_ctx));
  s_state = PLAYER_IDLE;
  s_ctx.need_config = true;
  s_ctx.codec = cfg->dec_cfg.codec;
  s_ctx.rate = cfg->i2s_cfg.rate;
  s_ctx.channels = 2;
  s_ctx.bits = 16;
  // alokacja dużych rzeczy TU, w s_ctx tylko wskaźniki
  s_ctx.buf_in = buf_create(cfg->buf_in_size);
  s_ctx.buf_out = buf_create(cfg->buf_out_size);
  s_ctx.dec = dec_create((dec_cfg_t *)&cfg->dec_cfg);
  s_ctx.dac = i2s_create((i2s_cfg_t *)&cfg->i2s_cfg);
  if (!s_ctx.buf_in || !s_ctx.buf_out || !s_ctx.dec || !s_ctx.dac) {
    return -1;
  }
  s_queue = xQueueCreate(5, sizeof(player_cmd_t));
  xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 7, NULL, 1);
  return 0;
}

buf_t player_get_input_buf(void) { return s_ctx.buf_in; }

int player_send_cmd(const player_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}

player_diag_t player_get_diag(void) {
  player_diag_t d = {0};
  d.buf_in_fill = buf_get_fill(s_ctx.buf_in);
  d.buf_out_fill = buf_get_fill(s_ctx.buf_out);
  d.dec = dec_get_diag(s_ctx.dec);
  d.i2s = i2s_get_diag(s_ctx.dac);
  return d;
}