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

typedef struct {
  net_rdo_cfg_t rdo;
  net_srv_cfg_t srv;
  bt_rcv_cfg_t bt;
  uint32_t mask;
  int fd;
} cfg_pend_t;

static StaticEventGroup_t s_app_eg_buf;
EventGroupHandle_t g_app_sync_evt = NULL;
static QueueHandle_t s_queue = NULL;
static app_state_t s_state, s_snap; // s_snap??? remove later ???
static cfg_pend_t s_pend;

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
    xEventGroupClearBits(g_app_sync_evt, EVT_BT_RCV_SHUTDOWN);
    bt_rcv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_BT_RCV_SHUTDOWN, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_RDO: {
    net_rdo_cmd_t cmd;
    cmd.type = NET_RDO_CMD_SHUTDOWN;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_SHUTDOWN);
    net_rdo_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_RDO_SHUTDOWN, timeout)) {
      return -1;
    }

    break;
  }

  case APP_SRC_NET_SRV: {
    net_srv_cmd_t cmd;
    cmd.type = NET_SRV_CMD_SHUTDOWN;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_SHUTDOWN);
    net_srv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_SRV_SHUTDOWN, timeout)) {
      return -1;
    }
    break;
  }

  default:
    break;
  }
  return 0;
}

static int app_src_play(app_src_t src) {
  if (s_state.is_playing) {
    return 0;
  }
  uint32_t timeout = 2000;
  switch (src) {
  case APP_SRC_NET_RDO: {
    net_rdo_cmd_t cmd;
    cmd.type = NET_RDO_CMD_PLAY;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_PLAYING);
    net_rdo_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_RDO_PLAYING, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_SRV: {
    net_srv_cmd_t cmd;
    cmd.type = NET_SRV_CMD_PLAY;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_PLAYING);
    net_srv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_SRV_PLAYING, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_BT_RCV: {
    bt_rcv_cmd_t cmd;
    cmd.type = BT_RCV_CMD_PLAY;
    xEventGroupClearBits(g_app_sync_evt, EVT_BT_RCV_PLAYING);
    bt_rcv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_BT_RCV_PLAYING, timeout)) {
      return -1;
    }
    break;
  }

  default:
    break;
  }

  s_state.is_playing = true;
  return 0;
}

static int app_src_stop(app_src_t src) {
  if (!s_state.is_playing) {
    return 0;
  }

  uint32_t timeout = 2000;
  switch (src) {
  case APP_SRC_NET_RDO: {
    net_rdo_cmd_t cmd;
    cmd.type = NET_RDO_CMD_STOP;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_STOPPED);
    net_rdo_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_RDO_STOPPED, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_SRV: {
    net_srv_cmd_t cmd;
    cmd.type = NET_SRV_CMD_STOP;
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_STOPPED);
    net_srv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_NET_SRV_STOPPED, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_BT_RCV: {
    bt_rcv_cmd_t cmd;
    cmd.type = BT_RCV_CMD_STOP;
    xEventGroupClearBits(g_app_sync_evt, EVT_BT_RCV_STOPPED);
    bt_rcv_send_cmd(&cmd);
    if (!app_wait_ack(EVT_BT_RCV_STOPPED, timeout)) {
      return -1;
    }
    break;
  }

  default:
    break;
  }
  s_state.is_playing = false;
  return 0;
}

static int app_src_start(app_src_t src) {
  const uint32_t timeout = 5000;
  switch (src) {
  case APP_SRC_NONE:
    break;

  case APP_SRC_NET_RDO: {
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_STARTED);
    int err = net_rdo_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_NET_RDO_STARTED, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_NET_SRV: {
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_STARTED);
    int err = net_srv_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_NET_SRV_STARTED, timeout)) {
      return -1;
    }
    break;
  }

  case APP_SRC_BT_RCV: {
    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_STARTED);
    int err = bt_rcv_start();
    if (err < 0) {
      return -1;
    }
    if (!app_wait_ack(EVT_BT_RCV_STARTED, timeout)) {
      return -1;
    }
    break;
  }
  }

  s_state.src_ready = false;
  return 0;
}

static int app_src_change(app_src_t new_src, app_state_t *st) {
  if (!st) {
    return -1;
  }
  if (st->is_playing) {
    int err = app_src_stop(st->src);
    if (err == 0) {
      ESP_LOGI(TAG, "stop ok!");
    }
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
  st->src_ready = false;
  return 0;
}

static int app_src_config(app_src_t src) {
  int timeout = 2000;
  switch (src) {
  case APP_SRC_NET_RDO: {
    s_state.net_rdo = s_pend.rdo;
    net_rdo_cmd_t rdo_cmd;
    memset(&rdo_cmd, 0, sizeof(rdo_cmd));
    rdo_cmd.type = NET_RDO_CMD_CONFIG;
    strlcpy(rdo_cmd.url, s_state.net_rdo.url, sizeof(rdo_cmd.url));

    xEventGroupClearBits(g_app_sync_evt, EVT_NET_RDO_READY);
    net_rdo_send_cmd(&rdo_cmd);
    if (!app_wait_ack(EVT_NET_RDO_READY, timeout)) {
      return -1;
    }
    break;
  }
  case APP_SRC_NET_SRV: {
    s_state.net_srv = s_pend.srv;
    net_srv_cmd_t srv_cmd;
    memset(&srv_cmd, 0, sizeof(srv_cmd));

    srv_cmd.type = NET_SRV_CMD_CONFIG;
    strlcpy(srv_cmd.srv_ip, s_state.net_srv.srv_ip, sizeof(srv_cmd.srv_ip));
    srv_cmd.srv_port = s_state.net_srv.srv_port;
    srv_cmd.format = s_state.net_srv.format;
    srv_cmd.rate = s_state.net_srv.rate;

    xEventGroupClearBits(g_app_sync_evt, EVT_NET_SRV_READY);
    net_srv_send_cmd(&srv_cmd);
    if (!app_wait_ack(EVT_NET_SRV_READY, timeout)) {
      return -1;
    }
    break;
  }
  case APP_SRC_BT_RCV: {
    s_state.bt_rcv = s_pend.bt;
    bt_rcv_cmd_t bt_cmd;
    memset(&bt_cmd, 0, sizeof(bt_cmd));

    bt_cmd.type = BT_RCV_CMD_CONFIG;
    bt_cmd.format = s_state.bt_rcv.format;
    bt_cmd.rate = s_state.bt_rcv.rate;

    xEventGroupClearBits(g_app_sync_evt, EVT_BT_RCV_READY);
    bt_rcv_send_cmd(&bt_cmd);
    if (!app_wait_ack(EVT_BT_RCV_READY, timeout)) {
      return -1;
    }
    break;
  }
  default:
    break;
  }

  s_pend.mask = 0;
  s_pend.fd = -1;
  s_state.src_ready = true; // could be bits not separate bools
  return 0;
}

static int app_cfg_ready(void) {
  ESP_LOGI(TAG, "entering config!");
  int ret = 0;
  bool was_playing = s_state.is_playing;
  if (was_playing) {
    ret = app_src_stop(s_state.src);
    if (ret == 0) {
      ESP_LOGI(TAG, "stop ok!");
    }
  }
  if (ret == 0) {
    s_state.src_ready = false;
    ret = app_src_config(s_state.src);
    if (ret == 0) {
      ESP_LOGI(TAG, "config ok!");
    }
  }
  if (was_playing && ret == 0) {
    ret = app_src_play(s_state.src);
    if (ret == 0) {
      ESP_LOGI(TAG, "play ok!");
    }
  }
  if (ret == 0) {
    return ret;
  }
  ESP_LOGE(TAG, "cfg : error!");
  s_pend.mask = 0;
  s_pend.fd = -1;
  return -1;
}

static void app_task(void *pvParameters) {
  bool cfg_ready = false;
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

      case APP_CMD_CONFIG_NET_RDO_URL:
        if (s_state.src != APP_SRC_NET_RDO) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        strlcpy(s_pend.rdo.url, cmd.data, sizeof(s_pend.rdo.url));
        s_pend.mask |= CFG_NET_RDO_URL;

        if ((s_pend.mask & CFG_NET_RDO_REQUIRED) == CFG_NET_RDO_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_CONFIG_NET_SRV_IP:
        if (s_state.src != APP_SRC_NET_SRV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        strlcpy(s_pend.srv.srv_ip, cmd.data, sizeof(s_pend.srv.srv_ip));
        s_pend.mask |= CFG_NET_SRV_IP;

        if ((s_pend.mask & CFG_NET_SRV_REQUIRED) == CFG_NET_SRV_REQUIRED) {
          cfg_ready = true;
        }

        break;

      case APP_CMD_CONFIG_NET_SRV_PORT:
        if (s_state.src != APP_SRC_NET_SRV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        s_pend.srv.srv_port = cmd.value;
        s_pend.mask |= CFG_NET_SRV_PORT;

        if ((s_pend.mask & CFG_NET_SRV_REQUIRED) == CFG_NET_SRV_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_CONFIG_NET_SRV_FORMAT:
        if (s_state.src != APP_SRC_NET_SRV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        s_pend.srv.format = cmd.value;
        s_pend.mask |= CFG_NET_SRV_FORMAT;

        if ((s_pend.mask & CFG_NET_SRV_REQUIRED) == CFG_NET_SRV_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_CONFIG_NET_SRV_RATE:
        if (s_state.src != APP_SRC_NET_SRV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        s_pend.srv.rate = cmd.value;
        s_pend.mask |= CFG_NET_SRV_RATE;

        if ((s_pend.mask & CFG_NET_SRV_REQUIRED) == CFG_NET_SRV_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_CONFIG_BT_RCV_FORMAT:
        if (s_state.src != APP_SRC_BT_RCV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        s_pend.bt.format = cmd.value;
        s_pend.mask |= CFG_BT_RCV_FORMAT;

        if ((s_pend.mask & CFG_BT_RCV_REQUIRED) == CFG_BT_RCV_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_CONFIG_BT_RCV_RATE:
        if (s_state.src != APP_SRC_BT_RCV) {
          ESP_LOGE(TAG, "wrong src!");
          break;
        }
        if (s_pend.fd == -1) {
          s_pend.fd = cmd.src_fd;
        }
        if (s_pend.fd != cmd.src_fd) {
          ESP_LOGE(TAG, "wrong fd!");
          break;
        }

        s_pend.bt.rate = cmd.value;
        s_pend.mask |= CFG_BT_RCV_RATE;

        if ((s_pend.mask & CFG_BT_RCV_REQUIRED) == CFG_BT_RCV_REQUIRED) {
          cfg_ready = true;
        }
        break;

      case APP_CMD_TOGGLE_PLAY:
        s_state.is_playing = !s_state.is_playing;
        need_broadcast = true;
        break;

      case APP_CMD_PLAY: {
        if (!s_state.src_ready) {
          ESP_LOGE(TAG, "not available!");
          break;
        }
        int err = app_src_play(s_state.src);
        if (err < 0) {
          ESP_LOGE(TAG, "error: play!");
          break;
        }
        ESP_LOGI(TAG, "play!");
        break;
      }

      case APP_CMD_STOP: {
        if (!s_state.src_ready) {
          ESP_LOGE(TAG, "not available!");
          break;
        }
        int err = app_src_stop(s_state.src);
        if (err < 0) {
          ESP_LOGE(TAG, "error: stop!");
          break;
        }
        ESP_LOGI(TAG, "stop!");
        break;
      }
      case APP_CMD_SET_VOL:
        s_state.volume = cmd.value;
        need_broadcast = true;
        break;

      default:
        break;
      }
    }
    s_snap = s_state; // ??? wtf

    if (cfg_ready) {
      int err = app_cfg_ready();
      if (err == 0) {
        ESP_LOGI(TAG, "cfg ready ok!");
      }
      cfg_ready = false;
    }

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
      .src_ready = false,
  };
  s_pend = (cfg_pend_t){
      .fd = -1,
      .mask = 0,
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