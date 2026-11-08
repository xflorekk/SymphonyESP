#include "net_rdo.h"
#include "app.h"
#include "app_sync.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "NET_RDO";

#define NET_RDO_STACK_SIZE 4096
#define NET_RDO_STACK_DEPTH (NET_RDO_STACK_SIZE / sizeof(StackType_t))

typedef enum {
  NET_RDO_TARGET_NONE = 0,
  NET_RDO_TARGET_CONFIG,
  NET_RDO_TARGET_STOP,
  NET_RDO_TARGET_PLAY,
} net_rdo_target_t;

typedef enum {
  NET_RDO_IDLE = 0,
  NET_RDO_CONFIGURING,
  NET_RDO_BUFFERING,
  NET_RDO_PLAYING,
  NET_RDO_SHUTDOWN,
  NET_RDO_OFF,
} net_rdo_state_t;

typedef struct {
  StackType_t stack[NET_RDO_STACK_DEPTH];
  StaticTask_t tcb;
  TaskHandle_t handle;
  volatile bool running;
} net_rdo_ctx_t;

static net_rdo_cfg_t s_cfg;
static net_rdo_ctx_t s_ctx;
static net_rdo_state_t s_state;
static net_rdo_target_t s_target;
static QueueHandle_t s_queue = NULL;

static void net_rdo_task(void *pvParameters) {
  s_state = NET_RDO_IDLE;
  ESP_LOGI(TAG, "task init!");
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_STARTED);

  net_rdo_cmd_t cmd;

  while (s_ctx.running) {
    // Queue Serivce
    TickType_t queue_wait = (s_state == NET_RDO_IDLE) ? portMAX_DELAY : 0;
    if (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {

      case NET_RDO_CMD_OK:
        break;

      case NET_RDO_CMD_CONFIG:
        ESP_LOGI(TAG, "config!");
        s_target = NET_RDO_TARGET_CONFIG;
        strlcpy(s_cfg.url, cmd.url, sizeof(s_cfg.url));
        ESP_LOGI(TAG, "url: %s", s_cfg.url);
        break;

      case NET_RDO_CMD_PLAY:
        s_target = NET_RDO_TARGET_PLAY;
        ESP_LOGI(TAG, "play!");
        break;

      case NET_RDO_CMD_STOP:
        s_target = NET_RDO_TARGET_STOP;
        ESP_LOGI(TAG, "stop!");
        break;

      case NET_RDO_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_state = NET_RDO_SHUTDOWN;
        // wait for player
        s_ctx.running = false;
        break;

      default:
        break;
      }
    }

    // State Machine
    switch (s_state) {

    case NET_RDO_IDLE:
      if (s_target == NET_RDO_TARGET_CONFIG) {
        // check ready bit, should be cleared!!!
        xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_READY);
        s_state = NET_RDO_CONFIGURING;
      } else if (s_target == NET_RDO_TARGET_PLAY) {
        // check READY bit?
        s_state = NET_RDO_BUFFERING;
        // send cmd to player!!!
        // wait for playing bit!!!
        //
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_PLAYING);
        ESP_LOGI(TAG, "going buffering!");
      }
      break;

    case NET_RDO_CONFIGURING:
      // get format and sample rate from URL, then send config to player!!!
      // wait for ack, if success set READY!!!

      s_state = NET_RDO_IDLE;
      s_target = NET_RDO_TARGET_NONE;
      ESP_LOGI(TAG, "config succes! going idle! net rdo ready!");
      xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_READY);
      break;

    case NET_RDO_BUFFERING:
      if (s_target == NET_RDO_TARGET_STOP) {
        // STOP the player
        // wait for the stop bit
        s_state = NET_RDO_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_RDO_PLAYING:
      if (s_target == NET_RDO_TARGET_STOP) {
        // STOP the player
        // wait for the stop bit
        s_state = NET_RDO_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    default:
      break;
    }
  }

  ESP_LOGI(TAG, "task cleanup!");
  // Cleanup
  s_ctx.handle = NULL;
  s_ctx.running = false;
  s_state = NET_RDO_OFF;
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_SHUTDOWN);
  vTaskDelete(NULL);
}

int net_rdo_init(void) {
  s_cfg = (net_rdo_cfg_t){
      .rate = 44100,
      .format = PLAYER_FORMAT_PCM,
  };
  s_state = NET_RDO_OFF;
  s_target = NET_RDO_TARGET_NONE;
  s_queue = xQueueCreate(5, sizeof(net_rdo_cmd_t));
  s_ctx.handle = NULL;
  s_ctx.running = false;
  return 0;
}

int net_rdo_start(void) {
  if (s_ctx.running) {
    return -1;
  }
  s_ctx.running = true;
  s_ctx.handle = xTaskCreateStaticPinnedToCore(net_rdo_task, "net_rdo",
                                               NET_RDO_STACK_DEPTH, NULL, 6,
                                               s_ctx.stack, &s_ctx.tcb, 0);
  if (!s_ctx.handle) {
    s_ctx.running = false;
    return -1;
  }
  return 0;
}

int net_rdo_stop(void) {
  if (!s_ctx.running) {
    return -1;
  }
  s_ctx.running = false;
  return 0;
}

int net_rdo_send_cmd(net_rdo_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}