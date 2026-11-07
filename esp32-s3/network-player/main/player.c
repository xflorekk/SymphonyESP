#include "player.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "PLAYER";

typedef enum {
  PLAYER_IDLE = 0,
  PLAYER_CONFIGURING,
  PLAYER_BUFFERING,
  PLAYER_PLAYING,
} player_state_t;

static player_state_t s_state;
static QueueHandle_t s_queue = NULL;

static void player_task(void *pvParameters) {
  player_cmd_t cmd;

  while (1) {

    TickType_t queue_wait = (s_state == PLAYER_IDLE) ? portMAX_DELAY : 0;
    while (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {

      switch (cmd.type) {
      case PLAYER_CMD_OK:
        break;

      default:
        break;
      }
    }

    switch (s_state) {

    case PLAYER_IDLE:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case PLAYER_CONFIGURING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case PLAYER_BUFFERING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case PLAYER_PLAYING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    default:
      break;
    }
  }
}

int player_init(void) {
  s_state = PLAYER_IDLE;
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