/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi.h
 * @brief WiFi STA driver, opaque handle, static storage, single event callback.
 *
 * @details
 * Design, same as player:
 * - no malloc, handle lives in storage provided by app,
 * - no globals, all state is in @ref wifi_s,
 * - one callback @ref wifi_on_event_cb instead of 5 separate callbacks,
 * - app only reacts to events, reconnect is done in wifi.c,
 * - `wifi_start` does init (if needed) + start + connect,
 *   `wifi_stop` is only stop, `wifi_destroy` is stop + deinit.
 *
 * Threads: `wifi_event_handler` in ESP event task updates `state` / `diag` /
 * `ip_str`, `wifi_get_*` functions are called from app task. Read is
 * best-effort, no mutex, enough for v1.
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

/**
 * @brief Size of storage for one handle.
 *
 * @details Must fit @ref wifi_s. Checked by `_Static_assert` in wifi.c.
 * If assert fails, increase this value.
 */
#define WIFI_STORAGE_SIZE 512

/**
 * @brief Static storage for handle.
 *
 * @details App allocates this statically, e.g. `static wifi_handle_storage_t
 * storage;` and passes it to @ref wifi_init. Content is opaque, do not touch
 * manually. 4-byte alignment required, struct contains pointers.
 */
typedef struct {
  uint8_t _opaque[WIFI_STORAGE_SIZE];
} __attribute__((aligned(4))) wifi_handle_storage_t;

/**
 * @brief Opaque WiFi handle.
 *
 * @details Real definition is only in wifi.c (`struct wifi_s`).
 * Use handle only via API in this file.
 */
typedef struct wifi_s *wifi_t;

/**
 * @brief WiFi error codes.
 */
typedef enum {
  WIFI_OK = 0, /**< Success. */
  WIFI_ERR_INVALID_ARG =
      -1, /**< Bad argument, e.g. NULL, empty SSID, SSID > 32, wrong mode. */
  WIFI_ERR_FAIL = -2, /**< ESP-IDF error, e.g. `esp_wifi_start` /
                         `esp_wifi_connect` failed. */
} wifi_err_t;

/**
 * @brief WiFi events sent to app via one callback.
 */
typedef enum {
  WIFI_EVT_CONNECTING = 0, /**< `WIFI_EVENT_STA_START`, trying to connect. */
  WIFI_EVT_CONNECTED,      /**< `WIFI_EVENT_STA_CONNECTED`, no IP yet. */
  WIFI_EVT_DISCONNECTED, /**< `WIFI_EVENT_STA_DISCONNECTED`, `data.reason` holds
                            ESP reason. */
  WIFI_EVT_GOT_IP,       /**< `IP_EVENT_STA_GOT_IP`, `data.ip` holds address. */
  WIFI_EVT_LOST_IP,      /**< `IP_EVENT_STA_LOST_IP`, `ip_str` cleared. */
  WIFI_EVT_FAIL, /**< `max_retry` exceeded, no auto-reconnect or max retry. */
} wifi_evt_t;

/**
 * @brief Event data.
 */
typedef struct {
  esp_ip4_addr_t ip; /**< Valid only for @ref WIFI_EVT_GOT_IP. */
  int reason;        /**< Valid only for @ref WIFI_EVT_DISCONNECTED,
                        `wifi_event_sta_disconnected_t.reason`. */
} wifi_evt_data_t;

/**
 * @brief Single WiFi event callback.
 *
 * @param ctx `user_ctx` from @ref wifi_cfg_t.
 * @param evt Event type.
 * @param data Event data, never NULL, may be empty `{0}`.
 *
 * @note Called from ESP event task, do not block, do not call `wifi_stop` /
 * `wifi_destroy` from callback. Use EventGroup / queue for sync in app.
 */
typedef void (*wifi_on_event_cb)(void *ctx, wifi_evt_t evt,
                                 const wifi_evt_data_t *data);

/**
 * @brief WiFi config, copied in @ref wifi_init.
 */
typedef struct {
  char ssid[64];     /**< SSID, 1..32 chars, validated in `wifi_init` /
                        `wifi_set_credentials`. */
  char password[64]; /**< Password, 0..64 chars, empty allowed for open network
                        (currently WPA2 forced anyway). */
  wifi_mode_t mode;  /**< Only @ref WIFI_MODE_STA in v1, other mode is
                        `WIFI_ERR_INVALID_ARG`. */
  wifi_ps_type_t
      ps; /**< Power save, `WIFI_PS_NONE` / `MIN_MODEM` / `MAX_MODEM`. */
  uint8_t max_retry; /**< Max reconnect attempts after `DISCONNECTED`, 0 = no
                        reconnect. */
  bool
      auto_reconnect; /**< Whether reconnect is done automatically in wifi.c. */

  wifi_on_event_cb on_event; /**< Event callback, may be NULL. */
  void *user_ctx;            /**< Passed as `ctx` to `on_event`. */
} wifi_cfg_t;

/**
 * @brief Diagnostics, copy, not pointer.
 *
 * @details Counters grow in event handler, `rssi` updated only by
 * @ref wifi_get_rssi. Cleared by @ref wifi_init and @ref wifi_reset_diag.
 */
typedef struct {
  uint32_t connect_cnt;    /**< How many times `WIFI_EVENT_STA_START`. */
  uint32_t disconnect_cnt; /**< How many times `WIFI_EVENT_STA_DISCONNECTED`. */
  uint32_t reconnect_cnt;  /**< How many times wifi.c did `esp_wifi_connect`
                              itself after disconnect. */
  int8_t rssi;        /**< Last read from `wifi_get_rssi`, 0 if no read yet. */
  uint8_t got_ip_cnt; /**< How many times `IP_EVENT_STA_GOT_IP`. */
} wifi_diag_t;

/**
 * @brief WiFi state.
 */
typedef enum {
  WIFI_STATE_IDLE =
      0, /**< After `init` / `stop` / `destroy`, before `start`. */
  WIFI_STATE_CONNECTING, /**< After `start`, waiting for `CONNECTED` / `GOT_IP`.
                          */
  WIFI_STATE_CONNECTED,  /**< Connected to AP, no IP. */
  WIFI_STATE_GOT_IP,     /**< Has IP, `wifi_get_ip` returns address. */
  WIFI_STATE_DISCONNECTED, /**< After `DISCONNECTED` / `LOST_IP`, before
                              reconnect or `IDLE`. */
  WIFI_STATE_ERROR, /**< After `max_retry` exceeded. Requires `stop` / `start`
                       or new credentials. */
} wifi_state_t;

/**
 * @brief Init handle in given storage.
 *
 * @param storage Storage from app, must outlive handle.
 * @param cfg Config, copied into handle.
 * @param out Output, handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG.
 *
 * @note Does not touch ESP, does not start WiFi. Only fills struct.
 */
wifi_err_t wifi_init(wifi_handle_storage_t *storage, const wifi_cfg_t *cfg,
                     wifi_t *out);

/**
 * @brief Deinit, stop + unregister + deinit + destroy netif.
 *
 * @param wifi Handle from @ref wifi_init.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG.
 *
 * @note After `destroy` handle is zeroed, `wifi_init` required again before
 * `wifi_start`. Event loop is left alone, it is global ESP loop.
 */
wifi_err_t wifi_destroy(wifi_t wifi);

/**
 * @brief Start WiFi, init (if needed) + start + connect.
 *
 * @param wifi Handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_FAIL.
 *
 * @note Idempotent, second `start` without `stop` returns `WIFI_OK`.
 * `WIFI_EVENT_STA_START` itself calls `esp_wifi_connect`, no need to call @ref
 * wifi_connect.
 */
wifi_err_t wifi_start(wifi_t wifi);

/**
 * @brief Stop WiFi, only `esp_wifi_stop`, no deinit.
 *
 * @param wifi Handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_FAIL.
 *
 * @note Sets `started=false` before `esp_wifi_stop`, so `DISCONNECTED` handler
 * does not do false reconnect. After `stop` state is `IDLE`,
 * `start` can be called again without `destroy`.
 */
wifi_err_t wifi_stop(wifi_t wifi);

/**
 * @brief Manual `esp_wifi_connect`.
 *
 * @param wifi Handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_FAIL.
 *
 * @note Usually not needed, `start` / reconnect does it.
 * Requires `started==true`.
 */
wifi_err_t wifi_connect(wifi_t wifi);

/**
 * @brief Manual `esp_wifi_disconnect`.
 *
 * @param wifi Handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_FAIL.
 *
 * @note Triggers `DISCONNECTED` and, if `auto_reconnect`, reconnect in handler.
 * Requires `started==true`.
 */
wifi_err_t wifi_disconnect(wifi_t wifi);

/**
 * @brief Change credentials.
 *
 * @param wifi Handle.
 * @param ssid New SSID, 1..32 chars.
 * @param password New password, 0..64 chars.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG / @ref WIFI_ERR_FAIL.
 *
 * @note If `started`, applies config immediately and does disconnect + connect.
 * If not started, only saves to `cfg`.
 */
wifi_err_t wifi_set_credentials(wifi_t wifi, const char *ssid,
                                const char *password);

/**
 * @brief Change mode.
 *
 * @param wifi Handle.
 * @param mode Only `WIFI_MODE_STA` in v1.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG / @ref WIFI_ERR_FAIL.
 *
 * @note Cannot change when `started`, `stop` first.
 * If `!initialized`, only saves to `cfg`, ESP gets it on `start`.
 */
wifi_err_t wifi_set_mode(wifi_t wifi, wifi_mode_t mode);

/**
 * @brief Change power save.
 *
 * @param wifi Handle.
 * @param ps_type `WIFI_PS_NONE` / `MIN_MODEM` / `MAX_MODEM`.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG / @ref WIFI_ERR_FAIL.
 *
 * @note If `!initialized`, only saves to `cfg`.
 */
wifi_err_t wifi_set_ps(wifi_t wifi, wifi_ps_type_t ps_type);

/**
 * @brief Get state.
 *
 * @param wifi Handle, NULL returns `WIFI_STATE_IDLE`.
 * @return Current @ref wifi_state_t.
 */
wifi_state_t wifi_get_state(wifi_t wifi);

/**
 * @brief Get IP as string.
 *
 * @param wifi Handle.
 * @param out_ip Output buffer.
 * @param out_size Buffer size, min 16 for `xxx.xxx.xxx.xxx`.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG / @ref WIFI_ERR_FAIL.
 *
 * @note Returns `WIFI_ERR_FAIL` and empty string if state !=
 * `WIFI_STATE_GOT_IP`.
 */
wifi_err_t wifi_get_ip(wifi_t wifi, char *out_ip, size_t out_size);

/**
 * @brief Get RSSI.
 *
 * @param wifi Handle.
 * @param out_rssi Output, dBm, e.g. -70.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG / @ref WIFI_ERR_FAIL.
 *
 * @note Calls `esp_wifi_sta_get_ap_info`, fails if not connected.
 * Also updates `diag.rssi`.
 */
wifi_err_t wifi_get_rssi(wifi_t wifi, int8_t *out_rssi);

/**
 * @brief Get copy of diagnostics.
 *
 * @param wifi Handle, NULL returns empty struct.
 * @return Copy of @ref wifi_diag_t.
 */
wifi_diag_t wifi_get_diag(wifi_t wifi);

/**
 * @brief Reset diagnostics.
 *
 * @param wifi Handle.
 * @return @ref WIFI_OK or @ref WIFI_ERR_INVALID_ARG.
 */
wifi_err_t wifi_reset_diag(wifi_t wifi);

#ifdef __cplusplus
}
#endif

#endif // WIFI_H