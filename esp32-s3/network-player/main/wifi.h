#ifndef WIFI_H
#define WIFI_H

#include <esp_err.h>
#include <esp_event.h>

ESP_EVENT_DECLARE_BASE(M_WIFI_EVE);

typedef enum {
  M_WIFI_EVE_STA_CONNECTING,
  M_WIFI_EVE_STA_CONNECTED,
  M_WIFI_EVE_STA_DISCONNECTED,
  M_WIFI_EVE_STA_GOT_IP,
  M_WIFI_EVE_STA_LOST_IP,
  M_WIFI_EVE_STA_FAIL,
} wifi_event_id_t;

#define WIFI_MAX_RETRY 5

int wifi_init(void);
esp_err_t wifi_start(void);
esp_err_t wifi_stop(void);

#endif