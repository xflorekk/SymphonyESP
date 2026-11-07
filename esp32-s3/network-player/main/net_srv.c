#include "net_srv.h"
#include "app.h"
#include "app_sync.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "NET_SRV";

#define NET_SRV_STACK_SIZE 4096
#define NET_SRV_STACK_DEPTH (NET_SRV_STACK_SIZE / sizeof(StackType_t))

typedef enum {
  NET_SRV_IDLE = 0,
  NET_SRV_CONFIGURING,
  NET_SRV_BUFFERING,
  NET_SRV_PLAYING,
  NET_SRV_OFF,
  NET_SRV_SHUTDOWN,
} net_srv_state_t;

typedef struct {
  StackType_t stack[NET_SRV_STACK_DEPTH];
  StaticTask_t tcb;
  TaskHandle_t handle;
  volatile bool running;
} net_srv_ctx_t;

static net_srv_ctx_t s_ctx;
static net_srv_state_t s_state;
static QueueHandle_t s_queue;

static void net_srv_task(void *pvParameters) {

  s_state = NET_SRV_IDLE;

  // clear the queue!!! after start!!!
  xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
  ESP_LOGI(TAG, "task_init!");
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STARTED);

  net_srv_cmd_t cmd;
  while (s_ctx.running) {

    TickType_t queue_wait = (s_state == NET_SRV_IDLE) ? portMAX_DELAY : 0;
    if (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {
      case NET_SRV_CMD_OK:
        break;

      case NET_SRV_CMD_SHUTDOWN:
        ESP_LOGI(TAG, "shutdown!");
        s_state = NET_SRV_SHUTDOWN;
        // wait for player
        s_ctx.running = false;
        break;

      default:
        break;
      }
    }

    switch (s_state) {
    case NET_SRV_IDLE:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_SRV_CONFIGURING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_SRV_BUFFERING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_SRV_PLAYING:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_SRV_OFF:
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    default:
      break;
    }
  }
  ESP_LOGI(TAG, "task cleanup!");
  s_ctx.handle = NULL;
  s_ctx.running = false;

  xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_STARTED);
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
  vTaskDelete(NULL);
}

int net_srv_init(void) {
  s_ctx.handle = NULL;
  s_ctx.running = false;
  s_state = NET_SRV_OFF;
  s_queue = xQueueCreate(5, sizeof(net_srv_cmd_t));
  return 0;
}

int net_srv_start(void) {
  if (s_ctx.running) {
    return -1;
  }
  s_ctx.running = true;
  s_ctx.handle = xTaskCreateStaticPinnedToCore(net_srv_task, "net_srv",
                                               NET_SRV_STACK_DEPTH, NULL, 7,
                                               s_ctx.stack, &s_ctx.tcb, 0);

  if (!s_ctx.handle) {
    s_ctx.running = false;
    return -1;
  }
  return 0;
}

int net_srv_stop(void) {
  if (!s_ctx.running) {
    return -1;
  }
  s_ctx.running = false;

  return 0;
}

int net_srv_send_cmd(net_srv_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}