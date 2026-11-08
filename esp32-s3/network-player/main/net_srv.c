#include "net_srv.h"
#include "app.h"
#include "app_sync.h"
#include "app_types.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "NET_SRV";

#define NET_SRV_STACK_SIZE 4096
#define NET_SRV_STACK_DEPTH (NET_SRV_STACK_SIZE / sizeof(StackType_t))

typedef enum {
  NET_SRV_TARGET_NONE = 0,
  NET_SRV_TARGET_CONFIG,
  NET_SRV_TARGET_STOP,
  NET_SRV_TARGET_PLAY,
} net_srv_target_t;

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

static net_srv_target_t s_target;
static net_srv_cfg_t s_cfg;
static net_srv_ctx_t s_ctx;
static net_srv_state_t s_state;
static QueueHandle_t s_queue;

static void net_srv_task(void *pvParameters) {

  s_state = NET_SRV_IDLE;
  xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STARTED);
  // clear the queue!!! after start!!!
  ESP_LOGI(TAG, "task_init!");

  net_srv_cmd_t cmd;
  while (s_ctx.running) {

    TickType_t queue_wait = (s_state == NET_SRV_IDLE) ? portMAX_DELAY : 0;
    if (xQueueReceive(s_queue, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {
      case NET_SRV_CMD_OK:
        break;

      case NET_SRV_CMD_CONFIG:
        ESP_LOGI(TAG, "config!");
        strlcpy(s_cfg.srv_ip, cmd.srv_ip, sizeof(s_cfg.srv_ip));
        s_cfg.srv_port = cmd.srv_port;
        s_cfg.format = cmd.format;
        s_cfg.rate = cmd.rate;
        s_target = NET_SRV_TARGET_CONFIG;
        ESP_LOGI(TAG, "srv_ip: %s, srv_port: %d, format: %d, rate: %d",
                 s_cfg.srv_ip, s_cfg.srv_port, s_cfg.format, s_cfg.rate);
        break;

      case NET_SRV_CMD_PLAY:
        ESP_LOGI(TAG, "play!");
        s_target = NET_SRV_TARGET_PLAY;
        break;

      case NET_SRV_CMD_STOP:
        ESP_LOGI(TAG, "stop!");
        s_target = NET_SRV_TARGET_STOP;
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
      if (s_target == NET_SRV_TARGET_CONFIG) {
        s_state = NET_SRV_CONFIGURING;
      } else if (s_target == NET_SRV_TARGET_PLAY) {
        s_state = NET_SRV_BUFFERING;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_PLAYING);
      } else if (s_target == NET_SRV_TARGET_STOP) {
        s_state = NET_SRV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
      }
      break;

    case NET_SRV_CONFIGURING:
      //
      s_state = NET_SRV_IDLE;
      s_target = NET_SRV_TARGET_NONE;
      ESP_LOGI(TAG, "config success! going idle! net srv ready!");
      xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_READY);
      break;

    case NET_SRV_BUFFERING:
      if (s_target == NET_SRV_TARGET_STOP) {
        // STOP the player
        // wait for the stop bit
        s_state = NET_SRV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    case NET_SRV_PLAYING:
      if (s_target == NET_SRV_TARGET_STOP) {
        // STOP the player
        // wait for the stop bit
        s_state = NET_SRV_IDLE;
        xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      break;

    default:
      break;
    }
  }
  ESP_LOGI(TAG, "task cleanup!");
  s_ctx.handle = NULL;
  s_ctx.running = false;

  xEventGroupSetBits(g_app_sync_evt, EVT_NET_SRV_SHUTDOWN);
  vTaskDelete(NULL);
}

int net_srv_init(void) {
  s_ctx.handle = NULL;
  s_ctx.running = false;
  s_target = NET_SRV_TARGET_NONE;
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