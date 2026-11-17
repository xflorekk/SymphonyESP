/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi.h"
#include "app_evt.h"
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WIFI";

/**
 * @brief Private definition of WIFI handle
 *
 * Defined only here, not in wifi.h
 */
struct wifi_s {
  wifi_cfg_t cfg;
  wifi_state_t state;
  wifi_diag_t diag;
  bool started;
  bool initialized;
  esp_netif_t *netif;
  uint8_t retry;
  char ip_str[16];
  esp_event_handler_instance_t wifi_handler;
  esp_event_handler_instance_t ip_handler;
};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  wifi_t wifi = (wifi_t)arg;
  if (!wifi) {
    return;
  }

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      wifi->state = WIFI_STATE_CONNECTING;
      wifi->diag.connect_cnt++;
      esp_wifi_connect();
      esp_event_post(APP_EVT, APP_EVT_WIFI_STA_CONNECTING, NULL, 0, 0);
      ESP_LOGI(TAG, "sta start, connecting to %s", wifi->cfg.ssid);
      break;

    case WIFI_EVENT_STA_CONNECTED:
      wifi->retry = 0;
      wifi->state = WIFI_STATE_CONNECTED;
      esp_event_post(APP_EVT, APP_EVT_WIFI_STA_CONNECTED, NULL, 0, 0);
      ESP_LOGI(TAG, "sta connected");
      break;

    case WIFI_EVENT_STA_DISCONNECTED: {
      wifi->state = WIFI_STATE_DISCONNECTED;
      wifi->diag.disconnect_cnt++;
      esp_event_post(APP_EVT, APP_EVT_WIFI_STA_DISCONNECTED, NULL, 0, 0);
      ESP_LOGW(TAG, "sta disconnected, retry %u/%u", wifi->retry,
               wifi->cfg.max_retry);
      if (wifi->cfg.auto_reconnect && wifi->retry < wifi->cfg.max_retry) {
        wifi->retry++;
        wifi->diag.reconnect_cnt++;
        // Reconnect is done here, in wifi.c. APP only reacts to event.
        esp_wifi_connect();
      } else {
        wifi->state = WIFI_STATE_ERROR;
        esp_event_post(APP_EVT, APP_EVT_WIFI_STA_FAIL, NULL, 0, 0);
        ESP_LOGE(TAG, "sta fail, max retry reached");
      }
      break;
    }

    default:
      break;
    }
  }

  if (event_base == IP_EVENT) {
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      wifi->retry = 0;
      wifi->state = WIFI_STATE_GOT_IP;
      wifi->diag.got_ip_cnt++;
      snprintf(wifi->ip_str, sizeof(wifi->ip_str), IPSTR,
               IP2STR(&event->ip_info.ip));
      esp_event_post(APP_EVT, APP_EVT_WIFI_STA_GOT_IP, &event->ip_info,
                     sizeof(event->ip_info), 0);
      ESP_LOGI(TAG, "got ip: %s", wifi->ip_str);
      break;
    }

    case IP_EVENT_STA_LOST_IP: {
      wifi->state = WIFI_STATE_DISCONNECTED;
      memset(wifi->ip_str, 0, sizeof(wifi->ip_str));
      esp_event_post(APP_EVT, APP_EVT_WIFI_STA_LOST_IP, NULL, 0, 0);
      ESP_LOGW(TAG, "lost ip");
      break;
    }

    default:
      break;
    }
  }
}

static esp_err_t wifi_apply_config(wifi_t wifi) {
  wifi_config_t wifi_config = {0};
  size_t ssid_len = strnlen(wifi->cfg.ssid, sizeof(wifi->cfg.ssid));
  size_t pass_len = strnlen(wifi->cfg.password, sizeof(wifi->cfg.password));
  memcpy(wifi_config.sta.ssid, wifi->cfg.ssid, ssid_len);
  memcpy(wifi_config.sta.password, wifi->cfg.password, pass_len);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;
  return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

wifi_t wifi_create(const wifi_cfg_t *cfg) {
  if (!cfg) {
    return NULL;
  }
  if (cfg->ssid[0] == '\0') {
    return NULL;
  }
  wifi_t w = (wifi_t)calloc(1, sizeof(struct wifi_s));
  if (!w) {
    return NULL;
  }
  w->cfg = *cfg;
  w->state = WIFI_STATE_IDLE;
  memset(&w->diag, 0, sizeof(w->diag));
  w->started = false;
  w->initialized = false;
  w->netif = NULL;
  w->retry = 0;
  memset(w->ip_str, 0, sizeof(w->ip_str));
  return w;
}

wifi_err_t wifi_destroy(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  wifi_stop(wifi);
  free(wifi);
  return WIFI_OK;
}

wifi_err_t wifi_start(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (wifi->started) {
    return WIFI_OK;
  }
  if (!wifi->initialized) {
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
      return WIFI_ERR_FAIL;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "event_loop_create failed: %s", esp_err_to_name(err));
      return WIFI_ERR_FAIL;
    }
    wifi->netif = esp_netif_create_default_wifi_sta();
    if (!wifi->netif) {
      return WIFI_ERR_FAIL;
    }
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) {
      return WIFI_ERR_FAIL;
    }
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, wifi,
                                            &wifi->wifi_handler) != ESP_OK) {
      return WIFI_ERR_FAIL;
    }
    if (esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, wifi,
                                            &wifi->ip_handler) != ESP_OK) {
      return WIFI_ERR_FAIL;
    }
    wifi->initialized = true;
  }

  if (esp_wifi_set_mode(wifi->cfg.mode) != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  if (wifi_apply_config(wifi) != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  if (esp_wifi_set_ps(wifi->cfg.ps) != ESP_OK) {
    ESP_LOGW(TAG, "esp_wifi_set_ps failed, continuing");
  }
  if (esp_wifi_start() != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  wifi->retry = 0;
  wifi->started = true;
  wifi->state = WIFI_STATE_CONNECTING;
  return WIFI_OK;
}

wifi_err_t wifi_stop(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (!wifi->started) {
    wifi->state = WIFI_STATE_IDLE;
    return WIFI_OK;
  }
  esp_wifi_stop();
  wifi->started = false;
  wifi->state = WIFI_STATE_IDLE;
  memset(wifi->ip_str, 0, sizeof(wifi->ip_str));
  return WIFI_OK;
}

wifi_err_t wifi_connect(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (!wifi->started) {
    return WIFI_ERR_FAIL;
  }
  if (esp_wifi_connect() != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  wifi->state = WIFI_STATE_CONNECTING;
  return WIFI_OK;
}

wifi_err_t wifi_disconnect(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (!wifi->started) {
    return WIFI_ERR_FAIL;
  }
  if (esp_wifi_disconnect() != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  wifi->state = WIFI_STATE_DISCONNECTED;
  return WIFI_OK;
}

wifi_err_t wifi_set_credentials(wifi_t wifi, const char *ssid,
                                const char *password) {
  if (!wifi || !ssid || !password) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (ssid[0] == '\0') {
    return WIFI_ERR_INVALID_ARG;
  }
  strncpy(wifi->cfg.ssid, ssid, sizeof(wifi->cfg.ssid) - 1);
  wifi->cfg.ssid[sizeof(wifi->cfg.ssid) - 1] = '\0';
  strncpy(wifi->cfg.password, password, sizeof(wifi->cfg.password) - 1);
  wifi->cfg.password[sizeof(wifi->cfg.password) - 1] = '\0';
  wifi->retry = 0;
  if (wifi->started) {
    // Apply new config and reconnect. APP only sends SET_WIFI, wifi.c does
    // reconnect.
    if (wifi_apply_config(wifi) != ESP_OK) {
      return WIFI_ERR_FAIL;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
    wifi->state = WIFI_STATE_CONNECTING;
  }
  return WIFI_OK;
}

wifi_err_t wifi_set_mode(wifi_t wifi, wifi_mode_t mode) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  // For now STA only. AP / APSTA is before commercialization step.
  // Keep function for API completeness, but do not switch in test.
  if (esp_wifi_set_mode(mode) != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  wifi->cfg.mode = mode;
  return WIFI_OK;
}

wifi_err_t wifi_set_ps(wifi_t wifi, wifi_ps_type_t ps_type) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (esp_wifi_set_ps(ps_type) != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  wifi->cfg.ps = ps_type;
  return WIFI_OK;
}

wifi_state_t wifi_get_state(wifi_t wifi) {
  if (!wifi) {
    return WIFI_STATE_IDLE;
  }
  return wifi->state;
}

wifi_err_t wifi_get_ip(wifi_t wifi, char *out_ip, size_t out_size) {
  if (!wifi || !out_ip || out_size == 0) {
    return WIFI_ERR_INVALID_ARG;
  }
  if (wifi->state != WIFI_STATE_GOT_IP) {
    out_ip[0] = '\0';
    return WIFI_ERR_FAIL;
  }
  strncpy(out_ip, wifi->ip_str, out_size - 1);
  out_ip[out_size - 1] = '\0';
  return WIFI_OK;
}

wifi_err_t wifi_get_rssi(wifi_t wifi, int8_t *out_rssi) {
  if (!wifi || !out_rssi) {
    return WIFI_ERR_INVALID_ARG;
  }
  wifi_ap_record_t ap = {0};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
    return WIFI_ERR_FAIL;
  }
  *out_rssi = ap.rssi;
  wifi->diag.rssi = ap.rssi;
  return WIFI_OK;
}

wifi_diag_t wifi_get_diag(wifi_t wifi) {
  wifi_diag_t empty = {0};
  if (!wifi) {
    return empty;
  }
  return wifi->diag;
}

wifi_err_t wifi_reset_diag(wifi_t wifi) {
  if (!wifi) {
    return WIFI_ERR_INVALID_ARG;
  }
  memset(&wifi->diag, 0, sizeof(wifi->diag));
  return WIFI_OK;
}