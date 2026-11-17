#include "app_evt.h"
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

static void main_nop(void) {
  ESP_LOGI(TAG, "nop!");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  if (app_evt_init() != ESP_OK) {
    ESP_LOGE(TAG, "app_evt_init FAIL");
    main_nop();
  }

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
  app_evt_set_srv(g_srv);

  wifi_cfg_t wifi_cfg = {
      .auto_reconnect = true,
      .max_retry = 5,
      .mode = WIFI_MODE_STA,
      .password = "XXX",
      .ssid = "XXX",
      .ps = WIFI_PS_NONE,
  };
  wifi_t wifi = wifi_create(&wifi_cfg);
  if (!wifi) {
    ESP_LOGE(TAG, "wifi_create FAIL");
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