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
  NET_RDO_IDLE = 0,
  NET_RDO_CONFIGURING,
  NET_RDO_BUFFERING,
  NET_RDO_PLAYING,
  NET_RDO_OFF,
  NET_RDO_SHUTDOWN,
} net_rdo_state_t;

typedef struct {
  StackType_t stack[NET_RDO_STACK_DEPTH];
  StaticTask_t tcb;
  TaskHandle_t handle;
  volatile bool running;
} net_rdo_ctx_t;

static net_rdo_ctx_t s_ctx;
static net_rdo_state_t s_state;
static QueueHandle_t s_queue = NULL;

static void net_rdo_task(void *pvParameters) {
  s_state = NET_RDO_IDLE;
  xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_STOPPED);
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

      case NET_RDO_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_state = NET_RDO_SHUTDOWN;
        s_ctx.running = false;
        break;

      default:
        break;
      }
    }

    // State Machine
    switch (s_state) {

    case NET_RDO_IDLE:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_RDO_CONFIGURING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_RDO_BUFFERING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_RDO_PLAYING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_RDO_OFF:
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
  xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_STARTED);
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_RDO_STOPPED);
  vTaskDelete(NULL);
}

int net_rdo_init(void) {
  s_state = NET_RDO_OFF;
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