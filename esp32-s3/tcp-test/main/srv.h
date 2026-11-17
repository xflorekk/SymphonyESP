/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SRV_H
#define SRV_H

#ifdef __cplusplus
extern "C" {
#endif

#include <freertos/FreeRTOS.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Handle is opaque.
 *
 * Created by srv_create(), destroyed by srv_destroy().
 * All state, including client pool, task stack and sockets,
 * is private in srv.c. Never dereference it.
 */
typedef struct srv_s *srv_t;

/**
 * @brief Server error codes.
 */
typedef enum {
  SRV_OK = 0,               /**< Success. */
  SRV_ERR_INVALID_ARG = -1, /**< NULL handle, NULL pointer or invalid config. */
  SRV_ERR_FAIL = -2,   /**< Generic failure, socket / bind / listen / task. */
  SRV_ERR_NO_MEM = -3, /**< Out of memory. */
  SRV_ERR_ALREADY_STARTED =
      -4, /**< srv_start() called while server is running. */
  SRV_ERR_NOT_STARTED =
      -5,                 /**< srv_stop() called while server is not running. */
  SRV_ERR_NO_CLIENT = -6, /**< No client with given fd. */
  SRV_ERR_WOULD_BLOCK =
      -7, /**< Send would block, client socket is non-blocking. */
} srv_err_t;

/**
 * @brief Called when a complete line is received from a client.
 *
 * @param user_ctx User context from srv_cfg_t.user_ctx.
 * @param fd Client socket descriptor, valid only during the call.
 * @param data One complete line, without trailing \r\n. Not necessarily
 *             NUL-terminated, always use @p len.
 * @param len Length of @p data in bytes, without trailing \r\n.
 *
 * @note Called in srv task context. Must not block, must not call
 *       srv_stop(), srv_destroy() or vTaskDelay(). Use srv_send_to()
 *       for a reply, it is thread-safe and non-blocking.
 */
typedef void (*srv_on_data_cb_t)(void *user_ctx, int fd, const char *data,
                                 size_t len);

/**
 * @brief Called when a client connects or disconnects.
 *
 * @param user_ctx User context from srv_cfg_t.user_ctx.
 * @param fd Client socket descriptor. In on_connect it is valid and can
 *           be used with srv_send_to() for a welcome message. In
 *           on_disconnect it is already closed and must not be used.
 *
 * @note Called in srv task context. Must not block, must not call
 *       srv_stop() or srv_destroy().
 */
typedef void (*srv_on_client_cb_t)(void *user_ctx, int fd);

/**
 * @brief Server configuration.
 *
 * Copied by srv_create(), caller may free its copy after the call.
 */
typedef struct {
  uint16_t port;            /**< TCP listen port. 0 is invalid. */
  uint8_t max_clients;      /**< Max simultaneous clients. 0 is invalid, 1..16
                               recommended. */
  int backlog;              /**< listen() backlog. 0 means default 5. */
  srv_on_data_cb_t on_data; /**< Called for each complete line. May be NULL,
                               lines are then dropped. */
  srv_on_client_cb_t on_connect;    /**< Called after accept(). May be NULL. */
  srv_on_client_cb_t on_disconnect; /**< Called after close(). May be NULL. */
  void *user_ctx;                   /**< Passed to all callbacks. */
  uint32_t stack_size;  /**< srv task stack in bytes. 0 means default 8192. */
  UBaseType_t priority; /**< srv task priority. 0 means default 5. */
  UBaseType_t core_id;  /**< Core for srv task, 0 or 1. */
  size_t rx_buf_size;   /**< Per-client RX line buffer in bytes. 0 means default
                           1024. Must be >= 128. */
  uint32_t idle_timeout_ms; /**< Disconnect client after this inactivity. 0
                               means disabled. */
} srv_cfg_t;

/**
 * @brief Server diagnostics.
 *
 * Copy returned by srv_get_diag(). Counters are updated from srv task
 * and from srv_send_to() / srv_send_all(), reads are thread-safe.
 */
typedef struct {
  uint32_t total_connections; /**< Total accepted connections since create or
                                 reset. */
  uint32_t current_clients;   /**< Currently connected clients. */
  uint32_t rx_bytes;          /**< Total bytes received. */
  uint32_t tx_bytes;          /**< Total bytes sent. */
  uint32_t rx_lines;          /**< Total complete lines delivered to on_data. */
  uint32_t dropped_lines; /**< Lines dropped, RX overflow or on_data == NULL. */
} srv_diag_t;

/**
 * @brief Create server instance.
 *
 * @param cfg Server configuration, must not be NULL. Copied internally.
 *
 * @return srv_t handle on success, NULL on invalid config or out of memory.
 *
 * @note Does not start listening. Call srv_start() after create.
 *       Allocates handle + client pool for cfg->max_clients.
 */
srv_t srv_create(const srv_cfg_t *cfg);

/**
 * @brief Destroy server instance.
 *
 * @param srv Server handle from srv_create().
 *
 * @return SRV_OK on success, SRV_ERR_INVALID_ARG if srv is NULL.
 *
 * @note If server is running, it is stopped first and the call blocks
 *       until srv task exits. Must not be called from srv callbacks,
 *       it would deadlock.
 */
srv_err_t srv_destroy(srv_t srv);

/**
 * @brief Start server.
 *
 * Creates srv task, opens listen socket, bind() and listen().
 * After start, server accepts clients and calls on_connect / on_data.
 *
 * @param srv Server handle.
 *
 * @return SRV_OK on success,
 *         SRV_ERR_INVALID_ARG if srv is NULL,
 *         SRV_ERR_ALREADY_STARTED if already running,
 *         SRV_ERR_FAIL if socket / bind / listen / task creation fails.
 */
srv_err_t srv_start(srv_t srv);

/**
 * @brief Stop server.
 *
 * Sets running flag, shutdown() listen socket and waits until srv task
 * exits and all client sockets are closed.
 *
 * @param srv Server handle.
 *
 * @return SRV_OK on success,
 *         SRV_ERR_INVALID_ARG if srv is NULL,
 *         SRV_ERR_NOT_STARTED if not running.
 *
 * @note Must not be called from srv callbacks, it would deadlock.
 */
srv_err_t srv_stop(srv_t srv);

/**
 * @brief Check if server is running.
 *
 * @param srv Server handle.
 *
 * @return true if srv_start() succeeded and srv_stop() not yet finished,
 *         false otherwise, including srv == NULL.
 */
bool srv_is_running(srv_t srv);

/**
 * @brief Send unicast message to a specific client.
 *
 * @param srv Server handle.
 * @param fd Client descriptor from on_connect / on_data.
 * @param data Data to send, may contain binary data.
 * @param len Length of @p data in bytes.
 *
 * @return SRV_OK on success,
 *         SRV_ERR_INVALID_ARG if srv is NULL or data is NULL,
 *         SRV_ERR_NOT_STARTED if server is not running,
 *         SRV_ERR_NO_CLIENT if fd is not connected,
 *         SRV_ERR_WOULD_BLOCK if client socket would block,
 *         SRV_ERR_FAIL on send error, client is then disconnected.
 *
 * @note Thread-safe, may be called from any task, including srv callbacks.
 *       Socket is non-blocking, send() never blocks the caller.
 *       Partial send is treated as success for the sent part, tx_bytes
 *       counts only sent bytes.
 */
srv_err_t srv_send_to(srv_t srv, int fd, const char *data, size_t len);

/**
 * @brief Send broadcast message to all connected clients.
 *
 * @param srv Server handle.
 * @param data Data to send, may contain binary data.
 * @param len Length of @p data in bytes.
 *
 * @return SRV_OK on success, even if no clients are connected,
 *         SRV_ERR_INVALID_ARG if srv is NULL or data is NULL,
 *         SRV_ERR_NOT_STARTED if server is not running,
 *         SRV_ERR_FAIL if all sends failed.
 *
 * @note Thread-safe, may be called from any task. Clients that fail
 *       with an error other than WOULD_BLOCK are disconnected.
 */
srv_err_t srv_send_all(srv_t srv, const char *data, size_t len);

/**
 * @brief Get a copy of diagnostics.
 *
 * @param srv Server handle.
 *
 * @return Copy of srv_diag_t. Returns zeroed struct if srv is NULL.
 *
 * @note Thread-safe.
 */
srv_diag_t srv_get_diag(srv_t srv);

/**
 * @brief Reset diagnostics.
 *
 * @param srv Server handle.
 *
 * @return SRV_OK on success, SRV_ERR_INVALID_ARG if srv is NULL.
 *
 * @note Thread-safe. Does not reset current_clients, it reflects
 *       real state, not a counter.
 */
srv_err_t srv_reset_diag(srv_t srv);

#ifdef __cplusplus
}
#endif

#endif // SRV_H