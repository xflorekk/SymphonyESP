#include "player.h"
#include "app_sync.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "PLAYER";

typedef enum {
  PLAYER_TARGET_STOPPED = 0,
  PLAYER_TARGET_PLAYING,
  PLAYER_TARGET_CONFIGURE,
} player_target_t;

typedef struct {
  bool target_playing;
  bool need_config;
  uint32_t rate;
  player_format_t format;
  player_target_t target;
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
        s_ctx.format = cmd.format;
        s_ctx.rate = cmd.rate;
        s_ctx.need_config = true;
        s_state = PLAYER_CONFIGURING;
        break;

      case PLAYER_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_state = PLAYER_SHUTDOWN;
        break;

      default:
        break;
      }
    }

    switch (s_state) {

    case PLAYER_IDLE:
      break;

    case PLAYER_CONFIGURING:
      ESP_LOGI(TAG, "configuring!");
      s_state = PLAYER_IDLE;
      xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_READY);
      break;

    case PLAYER_BUFFERING:
      break;

    case PLAYER_PLAYING:
      break;

    case PLAYER_SHUTDOWN:
      // Player Reset
      s_ctx.need_config = true;
      s_state = PLAYER_IDLE;
      xEventGroupSetBits(g_app_sync_evt, EVT_PLAYER_STOPPED);
      break;

    default:
      break;
    }
  }
}

int player_init(void) {
  s_state = PLAYER_IDLE;
  s_ctx = (player_ctx_t){
      .need_config = true,
      .target_playing = false,
      .rate = 44100,
      .format = PLAYER_FORMAT_PCM,
  };
  s_queue = xQueueCreate(5, sizeof(player_cmd_t));
  xTaskCreatePinnedToCore(player_task, "player", 8192, NULL, 7, NULL, 1);
  return 0;
}

int player_send_cmd(const player_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}