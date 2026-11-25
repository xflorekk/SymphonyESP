#include "wifi.h"
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>

static const char *TAG = "main";
static wifi_handle_storage_t storage;
static EventGroupHandle_t s_wifi_evt_group;

#define BIT_GOT_IP BIT0
#define BIT_FAIL BIT1

void main_nop(void) {
  ESP_LOGI(TAG, "nop!");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void on_event(void *ctx, wifi_evt_t evt,
                     const wifi_evt_data_t *evt_data) {
  (void)ctx;
  (void)evt_data;
  switch (evt) {
  case WIFI_EVT_CONNECTING:
    ESP_LOGI(TAG, "wifi CONNECTING");
    break;
  case WIFI_EVT_CONNECTED:
    ESP_LOGI(TAG, "wifi CONNECTED");
    break;
  case WIFI_EVT_DISCONNECTED:
    ESP_LOGI(TAG, "wifi DISCONNECTED");
    break;
  case WIFI_EVT_GOT_IP:
    ESP_LOGI(TAG, "wifi GOT IP");
    xEventGroupSetBits(s_wifi_evt_group, BIT_GOT_IP);
    break;
  case WIFI_EVT_LOST_IP:
    ESP_LOGI(TAG, "wifi LOST IP");
    break;
  case WIFI_EVT_FAIL:
    ESP_LOGI(TAG, "wifi FAIL");
    xEventGroupSetBits(s_wifi_evt_group, BIT_FAIL);
    break;
  default:
    break;
  }
}

static bool wait_got_ip(uint32_t timeout_ms) {
  EventBits_t bits =
      xEventGroupWaitBits(s_wifi_evt_group, BIT_GOT_IP | BIT_FAIL, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  return (bits & BIT_GOT_IP) != 0;
}

static void log_diag(wifi_t wifi) {
  char ip[16] = {0};
  wifi_get_ip(wifi, ip, sizeof(ip));
  int8_t rssi = 0;
  wifi_get_rssi(wifi, &rssi);
  wifi_diag_t d = wifi_get_diag(wifi);
  ESP_LOGI(TAG,
           "state=%d ip=%s rssi=%d connect=%lu disconnect=%lu reconnect=%lu "
           "got_ip=%u",
           wifi_get_state(wifi), ip, rssi, (unsigned long)d.connect_cnt,
           (unsigned long)d.disconnect_cnt, (unsigned long)d.reconnect_cnt,
           d.got_ip_cnt);
}

static void log_heap(const char *where) {
  ESP_LOGI(TAG,
           "%s heap free=%lu min=%lu internal=%lu psram=%lu largest=%lu "
           "stack_hwm=%u",
           where, (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size(),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void app_main(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  } else {
    ESP_ERROR_CHECK(err);
  }

  // Bez esp_event_loop_create_default() tutaj, loop robi wifi_start
  s_wifi_evt_group = xEventGroupCreate();
  if (!s_wifi_evt_group) {
    ESP_LOGE(TAG, "evt group FAIL");
    main_nop();
  }

  log_heap("BOOT");

  // TEST ONLY, przed commitem do menuconfig / NVS
  wifi_cfg_t cfg = (wifi_cfg_t){
      .auto_reconnect = true,
      .max_retry = 5,
      .mode = WIFI_MODE_STA,
      .password = "28567911",
      .ssid = "fiberway.pl_6c",
      .ps = WIFI_PS_NONE,
      .on_event = on_event,
      .user_ctx = NULL,
  };

  wifi_t wifi = NULL;
  wifi_err_t werr = wifi_init(&storage, &cfg, &wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "init FAIL");
    main_nop();
  }
  log_heap("INIT");

  // TEST 1: start -> GOT_IP
  ESP_LOGI(TAG, "TEST1 start -> GOT_IP");
  xEventGroupClearBits(s_wifi_evt_group, BIT_GOT_IP | BIT_FAIL);
  werr = wifi_start(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST1 start FAIL");
    main_nop();
  }
  if (!wait_got_ip(15000)) {
    ESP_LOGE(TAG, "TEST1 FAIL no GOT_IP");
    main_nop();
  }
  log_diag(wifi);
  log_heap("TEST1");

  // TEST 2: stop -> start, bez destroy
  ESP_LOGI(TAG, "TEST2 stop -> start");
  werr = wifi_stop(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST2 stop FAIL");
    main_nop();
  }
  log_heap("TEST2-STOP");
  vTaskDelay(pdMS_TO_TICKS(1000));
  xEventGroupClearBits(s_wifi_evt_group, BIT_GOT_IP | BIT_FAIL);
  werr = wifi_start(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST2 start FAIL");
    main_nop();
  }
  if (!wait_got_ip(15000)) {
    ESP_LOGE(TAG, "TEST2 FAIL no GOT_IP");
    main_nop();
  }
  log_diag(wifi);
  log_heap("TEST2");

  // TEST 3: stop -> destroy -> init -> start
  ESP_LOGI(TAG, "TEST3 destroy -> init -> start");
  werr = wifi_stop(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST3 stop FAIL");
    main_nop();
  }
  werr = wifi_destroy(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST3 destroy FAIL");
    main_nop();
  }
  log_heap("TEST3-DESTROY");
  werr = wifi_init(&storage, &cfg, &wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST3 init FAIL");
    main_nop();
  }
  xEventGroupClearBits(s_wifi_evt_group, BIT_GOT_IP | BIT_FAIL);
  werr = wifi_start(wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "TEST3 start FAIL");
    main_nop();
  }
  if (!wait_got_ip(15000)) {
    ESP_LOGE(TAG, "TEST3 FAIL no GOT_IP");
    main_nop();
  }
  log_diag(wifi);
  log_heap("TEST3");

  ESP_LOGI(TAG, "ALL WIFI TESTS PASS");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    log_diag(wifi);
    log_heap("LOOP");
  }
}