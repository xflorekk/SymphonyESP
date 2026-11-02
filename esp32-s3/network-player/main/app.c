#include "app.h"
#include "json.h"
#include "tcp_srv.h"
#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdbool.h>

static const char *TAG = "APP";

static QueueHandle_t s_app_queue = NULL;
static app_state_t s_state, s_snap;
static SemaphoreHandle_t s_lock = NULL;

int app_get_state(app_state_t *out) {
  if (!out) {
    return -1;
  }
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *out = s_state;
  xSemaphoreGive(s_lock);

  return 0;
}

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

static void app_task(void *pvParameters) {

  app_cmd_t cmd;
  char out[512];

  while (1) {

    bool need_broadcast = false;
    bool need_unicast = false;
    if (xQueueReceive(s_app_queue, &cmd, portMAX_DELAY) == pdTRUE) {

      xSemaphoreTake(s_lock, portMAX_DELAY);
      switch (cmd.type) {
      case APP_CMD_GET_STATE:
        need_unicast = true;
        break;

      case APP_CMD_TOGGLE_PLAY:
        s_state.is_playing = !s_state.is_playing;
        need_broadcast = true;
        break;

      case APP_CMD_SET_VOL:
        s_state.volume = cmd.value;
        need_broadcast = true;
        break;

      case APP_CMD_SET_URL:
        strlcpy(s_state.url, cmd.data, sizeof(s_state.url));
        need_broadcast = true;
        break;

      default:
        break;
      }
    }
    s_snap = s_state;
    xSemaphoreGive(s_lock);

    if (need_unicast) {
      json_build_state(out, sizeof(out), &s_snap);
      tcp_srv_unicast(cmd.src_fd, out);
    }
    if (need_broadcast) {
      json_build_broadcast(out, sizeof(out), &s_snap, cmd.type);
      tcp_srv_broadcast(out);
    }
    // broadcast response / unicast error / unicast state
  }
}

int app_init(void) {

  s_state = (app_state_t){
      .volume = 50,
      .is_muted = false,
      .is_playing = false,
  };
  s_app_queue = xQueueCreate(20, sizeof(app_cmd_t));
  s_lock = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 5, NULL, 0);
  esp_event_handler_register(M_WIFI_EVE, ESP_EVENT_ANY_ID, app_event_handler,
                             NULL);

  return 0;
}

int app_send_cmd(const app_cmd_t *cmd) {
  if (!s_app_queue || !cmd) {
    return -1;
  }
  return xQueueSend(s_app_queue, cmd, pdMS_TO_TICKS(20)) == pdTRUE ? 0 : -1;
}