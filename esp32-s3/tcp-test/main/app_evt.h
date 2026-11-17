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

#include "srv.h"
#include <esp_err.h>
#include <esp_event.h>
#include <esp_event_base.h>

ESP_EVENT_DECLARE_BASE(APP_EVT);

typedef enum {
  APP_EVT_WIFI_STA_CONNECTING,
  APP_EVT_WIFI_STA_CONNECTED,
  APP_EVT_WIFI_STA_DISCONNECTED,
  APP_EVT_WIFI_STA_GOT_IP,
  APP_EVT_WIFI_STA_LOST_IP,
  APP_EVT_WIFI_STA_FAIL,
} app_evt_id_t;

esp_err_t app_evt_init(void);

/**
 * @brief Give app_evt handle to srv, so GOT_IP can start it.
 * Must be called before wifi_start().
 */
void app_evt_set_srv(srv_t srv);

#ifdef __cplusplus
}
#endif

#endif // APP_EVT_H