#include "app_evt.h"
#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "main";

void main_nop(void) {
  ESP_LOGI(TAG, "nop!");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main(void) {

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_err_t err = app_evt_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Couldn't register handler!");
    main_nop();
  }

  wifi_cfg_t cfg;
  cfg = (wifi_cfg_t){
      .auto_reconnect = true,
      .max_retry = 5,
      .mode = WIFI_MODE_STA,
      .password = "28567911",
      .ssid = "fiberway.pl_6c",
      .ps = WIFI_PS_NONE,
  };

  wifi_t wifi = wifi_create(&cfg);
  if (!wifi) {
    ESP_LOGE(TAG, "wifi_create FAIL!");
    main_nop();
  }

  wifi_err_t werr = wifi_start(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "Couldn't start wifi!");
    main_nop();
  }
}
