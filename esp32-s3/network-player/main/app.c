#include "app.h"
#include "app_sync.h"
#include "bt_rcv.h"
#include "json.h"
#include "net_rdo.h"
#include "net_srv.h"
#include "tcp_srv.h"
#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdbool.h>

static const char *TAG = "APP";

static StaticEventGroup_t s_app_eg_buf;
EventGroupHandle_t g_app_sync_evt = NULL;
static QueueHandle_t s_queue = NULL;
static app_state_t s_state, s_snap;

static void app_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data) {

  if (base == M_WIFI_EVE) { // EVT
    switch (id) {
    case M_WIFI_EVE_STA_GOT_IP:
      if (tcp_srv_start() == 0) {
        ESP_LOGI(TAG, "tcp srv started!");
      }
      ESP_LOGI(TAG, "got ip!");
      break;

    default:
      break;
    }
  }
}

static bool app_wait_ack(EventBits_t bits, uint32_t timeout_ms) {
  EventBits_t ret = xEventGroupWaitBits(g_app_sync_evt, bits, pdTRUE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
  return (ret & bits) != 0;
}

static int app_src_shutdown(app_src_t src) {
  uint32_t timeout = 2000;
  switch (src) {

  case APP_SRC_NONE:
    break;

  case APP_SRC_BT_RCV: {
    bt_rcv_cmd_t cmd;
    cmd.type = BT_RCV_CMD_SHUTDOWN;
    bt_rcv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_BT_RCV_STOPPED, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_RDO: {
    net_rdo_cmd_t cmd;
    cmd.type = NET_RDO_CMD_SHUTDOWN;
    net_rdo_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_RDO_STOPPED, timeout)) {
      return -1;
    }

    break;
  }

  case APP_SRC_NET_SRV: {
    net_srv_cmd_t cmd;
    cmd.type = NET_SRV_CMD_SHUTDOWN;
    net_srv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_SRV_STOPPED, timeout)) {
      return -1;
    }
    break;
  }

  default:
    break;
  }
  return 0;
}

static int app_src_start(app_src_t src) {
  const uint32_t timeout = 5000;
  switch (src) {
  case APP_SRC_NONE:
    break;

  case APP_SRC_NET_RDO: {
    int err = net_rdo_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_NET_RDO_STARTED | EVT_NET_RDO_ERROR, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_SRV: {
    int err = net_srv_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_NET_SRV_STARTED | EVT_NET_SRV_ERROR, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_BT_RCV: {
    int err = bt_rcv_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_BT_RCV_STARTED | EVT_BT_RCV_ERROR, timeout)) {
      return -1;
    }
    break;
  }
  }

  return 0;
}

static int app_src_change(app_src_t new_src, app_state_t *st) {
  if (!st) {
    return -1;
  }
  if (new_src != st->src) {
    int err = app_src_shutdown(st->src);
    if (err < 0) {
      return -1;
    }

    err = app_src_start(new_src);
    if (err < 0) {
      st->src = APP_SRC_NONE;
      return -1;
    }
  }

  st->src = new_src;
  return 0;
}

static void app_task(void *pvParameters) {

  app_cmd_t cmd;
  char out[512];

  while (1) {

    bool need_broadcast = false;
    bool need_unicast = false;
    if (xQueueReceive(s_queue, &cmd, portMAX_DELAY) == pdTRUE) {

      switch (cmd.type) {
      case APP_CMD_GET_STATE:
        need_unicast = true;
        break;

      case APP_CMD_SET_SRC_NET_RADIO: {
        app_src_t src = APP_SRC_NET_RDO;
        ESP_LOGI(TAG, "set src net rdo!");
        int err = app_src_change(APP_SRC_NET_RDO, &s_state);
        if (err == 0) {
          ESP_LOGI(TAG, "ok");
          need_broadcast = true;
        } else {
          ESP_LOGE(TAG, "error");
          need_unicast = true; // error
        }
        break;
      }

      case APP_CMD_SET_SRC_NET_SERVER: {
        app_src_t src = APP_SRC_NET_SRV;
        ESP_LOGI(TAG, "set src net srv!");
        int err = app_src_change(APP_SRC_NET_SRV, &s_state);
        if (err == 0) {
          ESP_LOGI(TAG, "ok");
          need_broadcast = true;
        } else {
          ESP_LOGE(TAG, "error");
          need_unicast = true; // error
        }
        break;
      }

      case APP_CMD_SET_SRC_BT: {
        app_src_t src = APP_SRC_BT_RCV;
        ESP_LOGI(TAG, "set src bt rcv!");
        int err = app_src_change(APP_SRC_BT_RCV, &s_state);
        if (err == 0) {
          ESP_LOGI(TAG, "ok");
          need_broadcast = true;
        } else {
          ESP_LOGE(TAG, "error");
          need_unicast = true; // error
        }
        break;
      }

      case APP_CMD_SET_URL:
        strlcpy(s_state.url, cmd.data, sizeof(s_state.url));
        need_broadcast = true;
        break;

      case APP_CMD_TOGGLE_PLAY:
        s_state.is_playing = !s_state.is_playing;
        need_broadcast = true;
        break;

      case APP_CMD_SET_VOL:
        s_state.volume = cmd.value;
        need_broadcast = true;
        break;

      default:
        break;
      }
    }
    s_snap = s_state;

    // broadcast response / unicast error / unicast state
    if (need_unicast) {
      json_build_state(out, sizeof(out), &s_snap);
      tcp_srv_unicast(cmd.src_fd, out);
    }
    if (need_broadcast) {
      json_build_broadcast(out, sizeof(out), &s_snap, cmd.type);
      tcp_srv_broadcast(out);
    }
  }
}

int app_init(void) {

  g_app_sync_evt = xEventGroupCreateStatic(&s_app_eg_buf);
  s_state = (app_state_t){
      .volume = 50,
      .is_muted = false,
      .is_playing = false,
      .src = APP_SRC_NONE,
  };
  s_queue = xQueueCreate(20, sizeof(app_cmd_t));

  int err = net_rdo_init();
  if (err < 0) {
    ESP_LOGE(TAG, "net rdo init error!");
  }
  err = net_srv_init();
  if (err < 0) {
    ESP_LOGE(TAG, "net srv init error!");
  }
  err = bt_rcv_init();
  if (err < 0) {
    ESP_LOGE(TAG, "bt rcv init error!");
  }

  xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 5, NULL, 0);
  esp_event_handler_register(M_WIFI_EVE, ESP_EVENT_ANY_ID, app_event_handler,
                             NULL);

  return 0;
}

int app_send_cmd(const app_cmd_t *cmd) {
  if (!s_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}