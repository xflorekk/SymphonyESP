#include "app.h"
#include "tcp_srv.h"
#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char *TAG = "APP";

static QueueHandle_t s_app_queue = NULL;

static void app_event_handler(void *arg, esp_event_base_t base, int32_t id,
                              void *data) {

  if (base == M_WIFI_EVE) {
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

  while (1) {

    if (xQueueReceive(s_app_queue, &cmd, portMAX_DELAY) == pdTRUE) {

      switch (cmd.type) {
      case APP_CMD_OK:
        break;

      default:
        break;
      }
    }
  }
}

int app_init(void) {
  s_app_queue = xQueueCreate(10, sizeof(app_cmd_t));
  xTaskCreatePinnedToCore(app_task, "app", 8192, NULL, 5, NULL, 0);
  esp_event_handler_register(M_WIFI_EVE, ESP_EVENT_ANY_ID, app_event_handler,
                             NULL);

  return 0;
}

int app_send_cmd(app_cmd_t *cmd) { return -1; }