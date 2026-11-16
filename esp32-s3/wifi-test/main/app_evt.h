/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef APP_EVT_H
#define APP_EVT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>
#include <esp_event.h>
#include <esp_event_base.h>

/**
 *@brief Declare events APP_EVT
 */
ESP_EVENT_DECLARE_BASE(APP_EVT);

/**
 *@brief APP EVT - events
 */
typedef enum {
  APP_EVT_WIFI_STA_CONNECTING,
  APP_EVT_WIFI_STA_CONNECTED,
  APP_EVT_WIFI_STA_DISCONNECTED,
  APP_EVT_WIFI_STA_GOT_IP,
  APP_EVT_WIFI_STA_LOST_IP,
  APP_EVT_WIFI_STA_FAIL,
} app_evt_id_t;

/**
 *@brief Init app_evt_handler() - gets queue handle
 */
esp_err_t app_evt_init(void);

#ifdef __cplusplus
}
#endif

#endif // APP_EVT_H