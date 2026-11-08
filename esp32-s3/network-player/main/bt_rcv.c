#include "bt_rcv.h"
#include "app.h"
#include "app_sync.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "BT_RCV";

#define BT_RCV_STACK_SIZE 3072
#define BT_RCV_STACK_DEPTH (BT_RCV_STACK_SIZE / sizeof(StackType_t))

typedef enum {
  BT_RCV_TARGET_NONE = 0,
  BT_RCV_TARGET_CONFIG,
  BT_RCV_TARGET_PLAY,
  BT_RCV_TARGET_STOP,
} bt_rcv_target_t;

typedef enum {
  BT_RCV_IDLE = 0,
  BT_RCV_CONFIGURING,
  BT_RCV_BUFFERING,
  BT_RCV_PLAYING,
  BT_RCV_OFF,
  BT_RCV_SHUTDOWN,
} bt_rcv_state_t;

typedef struct {
  StackType_t stack[BT_RCV_STACK_DEPTH];
  StaticTask_t tcb;
  TaskHandle_t handle;
  volatile bool running;
} bt_rcv_ctx_t;

static bt_rcv_target_t s_target;
static bt_rcv_cfg_t s_cfg;
// later put these into struct
static bt_rcv_ctx_t s_ctx;
static bt_rcv_state_t s_state;
static QueueHandle_t s_queue = NULL;

static void bt_rcv_task(void *pvParameters) {
  s_state = BT_RCV_IDLE;
  // clear the queue!!!
  xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_STARTED);

  ESP_LOGI(TAG, "task init!");
  bt_rcv_cmd_t cmd;

  while (s_ctx.running) {

    TickType_t queue_wait = (s_state == BT_RCV_IDLE) ? portMAX_DELAY : 0;

    if (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {
      case BT_RCV_CMD_OK:
        break;

      case BT_RCV_CMD_CONFIG:
        ESP_LOGI(TAG, "config!");
        s_cfg.format = cmd.format;
        s_cfg.rate = cmd.rate;
        s_target = BT_RCV_TARGET_CONFIG;
        ESP_LOGI(TAG, "format: %d, rate: %d", s_cfg.format, s_cfg.rate);
        break;

      case BT_RCV_CMD_PLAY:
        ESP_LOGI(TAG, "play!");
        s_target = BT_RCV_TARGET_PLAY;
        break;

      case BT_RCV_CMD_STOP:
        ESP_LOGI(TAG, "stop!");
        s_target = BT_RCV_TARGET_STOP;
        break;

      case BT_RCV_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_state = BT_RCV_SHUTDOWN;
        s_ctx.running = false;
        break;

      default:
        break;
      }
    }

    switch (s_state) {
    case BT_RCV_IDLE:
      if (s_target == BT_RCV_TARGET_CONFIG) {
        s_state = BT_RCV_CONFIGURING;
      } else if (s_target == BT_RCV_TARGET_PLAY) {
        s_state = BT_RCV_BUFFERING;
        xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_PLAYING);
      } else if (s_target == BT_RCV_TARGET_STOP) {
        s_state = BT_RCV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_STOPPED);
      }
      break;

    case BT_RCV_CONFIGURING:
      s_state = BT_RCV_IDLE;
      s_target = BT_RCV_TARGET_NONE;
      ESP_LOGI(TAG, "config succes! going idle! bt rcv ready!");
      xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_READY);
      break;

    case BT_RCV_BUFFERING:
      if (s_target == BT_RCV_TARGET_STOP) {
        s_state = BT_RCV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case BT_RCV_PLAYING:
      if (s_target == BT_RCV_TARGET_STOP) {
        s_state = BT_RCV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case BT_RCV_SHUTDOWN:
      break;

    default:
      break;
    }
  }

  ESP_LOGI(TAG, "task cleanup!");

  s_ctx.handle = NULL;
  s_ctx.running = false;
  s_state = BT_RCV_OFF;
  s_target = BT_RCV_TARGET_NONE;

  xEventGroupSetBits(g_app_sync_evt, EVT_BT_RCV_SHUTDOWN);
  vTaskDelete(NULL);
}

int bt_rcv_init(void) {
  s_target = BT_RCV_TARGET_NONE;
  s_state = BT_RCV_OFF;
  s_queue = xQueueCreate(5, sizeof(bt_rcv_cmd_t));
  s_ctx.handle = NULL;
  s_ctx.running = false;
  return 0;
}

int bt_rcv_start(void) {
  if (s_ctx.running) {
    return -1;
  }
  s_ctx.running = true;
  s_ctx.handle =
      xTaskCreateStaticPinnedToCore(bt_rcv_task, "bt_rcv", BT_RCV_STACK_DEPTH,
                                    NULL, 6, s_ctx.stack, &s_ctx.tcb, 0);

  if (!s_ctx.handle) {
    s_ctx.running = false;
    return -1;
  }
  return 0;
}

int bt_rcv_stop(void) {
  if (!s_ctx.running) {
    return -1;
  }
  s_ctx.running = false;
  return 0;
}

int bt_rcv_send_cmd(bt_rcv_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}