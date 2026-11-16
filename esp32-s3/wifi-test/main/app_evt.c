/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_evt.h"
#include <esp_log.h>
#include <esp_netif.h>

static const char *TAG = "app_evt";

ESP_EVENT_DEFINE_BASE(APP_EVT);

static void app_evt_handler(void *arg, esp_event_base_t base, int32_t id,
                            void *data) {

  switch (id) {

  case APP_EVT_WIFI_STA_CONNECTED:
    ESP_LOGI(TAG, "wifi connected!");
    break;

  case APP_EVT_WIFI_STA_DISCONNECTED:
    ESP_LOGI(TAG, "wifi disconnected!");
    break;

  case APP_EVT_WIFI_STA_GOT_IP:
    esp_netif_ip_info_t *ip = (esp_netif_ip_info_t *)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ip->ip));
    break;

  case APP_EVT_WIFI_STA_LOST_IP:
    ESP_LOGI(TAG, "lost ip!");
    break;
  }

  // send cmd
}

esp_err_t app_evt_init(void) {
  return esp_event_handler_instance_register(APP_EVT, ESP_EVENT_ANY_ID,
                                             &app_evt_handler, NULL, NULL);
}
