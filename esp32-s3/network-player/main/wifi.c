#include "wifi.h"
#include "wifi_secret.h"
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
static uint8_t s_retry_num = 0;
static esp_netif_t *s_sta_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

ESP_EVENT_DEFINE_BASE(M_WIFI_EVE);

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {

  if (event_base == WIFI_EVENT) {
    switch (event_id) {

    case WIFI_EVENT_STA_START:
      esp_wifi_connect();
      esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_CONNECTING, NULL, 0, 0);
      ESP_LOGI(TAG, "sta start!");
      break;

    case WIFI_EVENT_STA_CONNECTED:
      s_retry_num = 0;
      esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_CONNECTED, NULL, 0, 0);
      ESP_LOGI(TAG, "sta connected!");
      break;

    case WIFI_EVENT_STA_DISCONNECTED:
      if (s_retry_num < WIFI_MAX_RETRY) {
        s_retry_num++;
        esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_DISCONNECTED, NULL, 0, 0);
      } else {
        esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_FAIL, NULL, 0, 0);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
      ESP_LOGW(TAG, "sta disconnected!");
      break;

    default:
      break;
    }
  }

  if (event_base == IP_EVENT) {
    switch (event_id) {

    case IP_EVENT_STA_GOT_IP: {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      s_retry_num = 0;
      xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_GOT_IP, &event->ip_info,
                     sizeof(event->ip_info), 0);
      ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
      break;
    }

    case IP_EVENT_STA_LOST_IP: {
      esp_event_post(M_WIFI_EVE, M_WIFI_EVE_STA_LOST_IP, NULL, 0, 0);
      ESP_LOGW(TAG, "lost ip!");
      break;
    }

    default:
      break;
    }
  }
}

int wifi_init(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  s_wifi_event_group = xEventGroupCreate();
  s_sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&config));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SECRET_SSID,
              .password = WIFI_SECRET_PASSWORD,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .pmf_cfg =
                  {
                      .capable = true,
                      .required = false,
                  },
          },
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  ESP_LOGI(TAG, "wifi init fin!");

  return 0;
}

esp_err_t wifi_start(void) {
  s_retry_num = 0;
  xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
  return esp_wifi_start();
}

esp_err_t wifi_stop(void) { return esp_wifi_stop(); }