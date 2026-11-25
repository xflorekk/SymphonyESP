#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "main";
static wifi_handle_storage_t storage;

void main_nop(void) {
  ESP_LOGI(TAG, "nop!");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void on_event(void *ctx, wifi_evt_t evt,
                     const wifi_evt_data_t *evt_data) {

  switch (evt) {
  case WIFI_EVT_DISCONNECTED:
    ESP_LOGI(TAG, "wifi DISCONNECTED");
    break;
  case WIFI_EVT_CONNECTED:
    ESP_LOGI(TAG, "wifi CONNECTED");
    break;
  case WIFI_EVT_CONNECTING:
    ESP_LOGI(TAG, "wifi CONNECTING");
    break;
  case WIFI_EVT_GOT_IP:
    ESP_LOGI(TAG, "wifi GOT IP");
    break;
  case WIFI_EVT_LOST_IP:
    ESP_LOGI(TAG, "wifi LOST IP");
    break;
  case WIFI_EVT_FAIL:
    ESP_LOGI(TAG, "wifi FAIL");
    break;
  default:
    break;
  }
}

void app_main(void) {

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  wifi_cfg_t cfg;
  cfg = (wifi_cfg_t){
      .auto_reconnect = true,
      .max_retry = 5,
      .mode = WIFI_MODE_STA,
      .password = "28567911",
      .ssid = "fiberway.pl_6c",
      .ps = WIFI_PS_NONE,
      .on_event = on_event,
      .user_ctx = NULL,
  };
  wifi_t wifi;
  wifi_err_t werr = wifi_init(&storage, &cfg, &wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "wifi_init FAIL!");
    main_nop();
  }

  werr = wifi_start(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "Couldn't start wifi!");
    main_nop();
  }
}
