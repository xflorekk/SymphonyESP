/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_wifi.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_STORAGE_SIZE 512

typedef struct {
  uint8_t _opaque[WIFI_STORAGE_SIZE];
} __attribute__((aligned(4))) wifi_handle_storage_t;

/**
 *@brief WIFI handle
 *
 * Handle is opaque...
 */
typedef struct wifi_s *wifi_t;

/**
 *@brief WIFI error codes
 */
typedef enum {
  WIFI_OK = 0,
  WIFI_ERR_INVALID_ARG = -1,
  WIFI_ERR_FAIL = -2,
} wifi_err_t;

typedef enum {
  WIFI_EVT_CONNECTING = 0,
  WIFI_EVT_CONNECTED,
  WIFI_EVT_DISCONNECTED,
  WIFI_EVT_GOT_IP,
  WIFI_EVT_LOST_IP,
  WIFI_EVT_FAIL,
} wifi_evt_t;

typedef struct {
  esp_ip4_addr_t ip;
  int reason;
} wifi_evt_data_t;

typedef void (*wifi_on_event_cb)(void *ctx, wifi_evt_t evt,
                                 const wifi_evt_data_t *data);

/**
 *@brief WIFI config
 */
typedef struct {
  char ssid[64];
  char password[64];
  wifi_mode_t mode;
  wifi_ps_type_t ps;
  uint8_t max_retry;
  bool auto_reconnect;

  wifi_on_event_cb on_event;
  void *user_ctx;
} wifi_cfg_t;

/**
 *@brief WIFI diagnostics
 */
typedef struct {
  uint32_t connect_cnt;
  uint32_t disconnect_cnt;
  uint32_t reconnect_cnt;
  int8_t rssi;
  uint8_t got_ip_cnt;
} wifi_diag_t;

/**
 *@brief WIFI state
 */

typedef enum {
  WIFI_STATE_IDLE = 0,
  WIFI_STATE_CONNECTING,
  WIFI_STATE_CONNECTED,
  WIFI_STATE_GOT_IP,
  WIFI_STATE_DISCONNECTED,
  WIFI_STATE_ERROR,
} wifi_state_t;

/**
 *@brief Init
 */
wifi_err_t wifi_init(wifi_handle_storage_t *storage, const wifi_cfg_t *cfg,
                     wifi_t *out);

/**
 *@brief Deinit
 */
wifi_err_t wifi_destroy(wifi_t wifi);

/**
 *@brief Start wifi
 *
 * Init + Start + Connect
 */
wifi_err_t wifi_start(wifi_t wifi);

/**
 *@brief Stop wifi
 *
 */
wifi_err_t wifi_stop(wifi_t wifi);

/**
 *@brief Connect wifi
 */
wifi_err_t wifi_connect(wifi_t wifi);

/**
 *@brief Disconnect wifi
 */
wifi_err_t wifi_disconnect(wifi_t wifi);

/**
 *@brief Set credentials
 */
wifi_err_t wifi_set_credentials(wifi_t wifi, const char *ssid,
                                const char *password);

/**
 *@brief Set mode
 */
wifi_err_t wifi_set_mode(wifi_t wifi, wifi_mode_t mode);

/**
 *@brief Set Power Saving (ps)
 */
wifi_err_t wifi_set_ps(wifi_t wifi, wifi_ps_type_t ps_type);

/**
 *@brief Get state
 */
wifi_state_t wifi_get_state(wifi_t wifi);

/**
 *@brief Get IP
 */
wifi_err_t wifi_get_ip(wifi_t wifi, char *out_ip, size_t out_size);

/**
 *@brief Get rssi (signal strength)
 */
wifi_err_t wifi_get_rssi(wifi_t wifi, int8_t *out_rssi);

/**
 *@brief Get a copy of diagnostics
 */
wifi_diag_t wifi_get_diag(wifi_t wifi);

/**
 *@brief Reset wifi diagnostics
 */
wifi_err_t wifi_reset_diag(wifi_t wifi);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H