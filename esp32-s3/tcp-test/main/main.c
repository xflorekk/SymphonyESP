#include "srv.h"
#include "wifi.h"
#include <esp_event.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <string.h>

static const char *TAG = "main";
static srv_t g_srv = NULL;
static wifi_handle_storage_t storage;

static void srv_on_connect(void *ctx, int fd) {
  (void)ctx;
  ESP_LOGI(TAG, "srv client connected fd=%d", fd);
  const char *welcome = "{\"type\":\"welcome\",\"msg\":\"hello from S3\"}\n";
  srv_send_to(g_srv, fd, welcome, strlen(welcome));
}

static void srv_on_disconnect(void *ctx, int fd) {
  (void)ctx;
  ESP_LOGI(TAG, "srv client disconnected fd=%d", fd);
}

static void srv_on_data(void *ctx, int fd, const char *data, size_t len) {
  (void)ctx;
  ESP_LOGI(TAG, "srv RX fd=%d len=%u: %.*s", fd, (unsigned)len, (int)len, data);
  // Echo test: odsyłamy to samo z prefixem
  char echo[1100];
  int n = snprintf(echo, sizeof(echo), "echo: %.*s\n", (int)len, data);
  if (n > 0) {
    srv_send_to(g_srv, fd, echo, (size_t)n);
  }
}

static void wifi_on_event(void *ctx, wifi_evt_t evt,
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

    srv_start(g_srv);
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

static void main_nop(void) {
  ESP_LOGI(TAG, "nop!");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());

  // SRV - tworzymy przed WiFi, ale startujemy dopiero na GOT_IP
  srv_cfg_t srv_cfg = {
      .port = 3333,
      .max_clients = 4,
      .backlog = 5,
      .on_data = srv_on_data,
      .on_connect = srv_on_connect,
      .on_disconnect = srv_on_disconnect,
      .user_ctx = NULL,
      .stack_size = 0, // default 8192
      .priority = 5,
      .core_id = 0,
      .rx_buf_size = 0,         // default 1024
      .idle_timeout_ms = 10000, // disabled na test
  };
  g_srv = srv_create(&srv_cfg);
  if (!g_srv) {
    ESP_LOGE(TAG, "srv_create FAIL");
    main_nop();
  }

  wifi_cfg_t cfg = (wifi_cfg_t){
      .auto_reconnect = true,
      .max_retry = 5,
      .mode = WIFI_MODE_STA,
      .password = "28567911",
      .ssid = "fiberway.pl_6c",
      .ps = WIFI_PS_NONE,
      .on_event = wifi_on_event,
      .user_ctx = NULL,
  };
  wifi_t wifi = NULL;
  wifi_err_t werr = wifi_init(&storage, &cfg, &wifi);
  if (werr != WIFI_OK) {
    ESP_LOGE(TAG, "init FAIL");
    main_nop();
  }
  if (wifi_start(wifi) != WIFI_OK) {
    ESP_LOGE(TAG, "wifi_start FAIL");
    main_nop();
  }

  // Nic więcej nie robimy, wszystko dzieje się w eventach + srv tasku
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}