/*
 * SPDX-FileCopyrightText: 2026 Maciek Pollok
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file srv.c
 * @brief TCP line server implementation, static allocation.
 *
 * @details Single srv task owns listen socket, select() loop and client
 * RX buffers. srv_send_to() / srv_send_all() may be called from any task,
 * they use the same mutex as the task. All state lives in
 * srv_handle_storage_t provided by the app, no heap allocation.
 */
#include "srv.h"
#include <arpa/inet.h>
#include <errno.h>
#include <esp_log.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "SRV";

#define SRV_DEFAULT_BACKLOG 5
#define SRV_SELECT_TIMEOUT_MS 1000
#define SRV_STOP_TIMEOUT_MS 2000
#define SRV_RECV_TMP_SIZE 256

/**
 * @brief Private client slot.
 *
 * Owned only by srv task, except srv_send_to() / srv_send_all()
 * which access it under lock. Never exposed outside srv.c.
 */
typedef struct {
  int fd;
  bool in_use;
  bool discarding;
  char rx_buf[SRV_RX_BUF_SIZE];
  size_t rx_len;
  TickType_t last_activity;
} srv_cli_t;

struct srv_s {
  StackType_t stack[SRV_STACK_DEPTH];
  StaticTask_t tcb;
  StaticSemaphore_t lock_buf;
  StaticSemaphore_t stopped_buf;
  StaticSemaphore_t started_buf;
  TaskHandle_t handle;
  int listen_fd;
  volatile bool running;
  SemaphoreHandle_t lock;
  SemaphoreHandle_t stopped_sem;
  SemaphoreHandle_t started_sem;
  bool start_ok;
  srv_cfg_t cfg;
  srv_diag_t diag;
  srv_cli_t cli[SRV_MAX_CLIENTS];
};

/** @brief Storage must fit `struct srv_s`, otherwise increase
 * `SRV_STORAGE_SIZE`. */
_Static_assert(sizeof(struct srv_s) <= sizeof(srv_handle_storage_t),
               "SRV_STORAGE_SIZE too small");

/* --- helpers, lock must be held unless noted --- */

/**
 * @brief Find free client slot.
 * @param srv Server handle.
 * @return Slot index or -1 if full. Lock must be held.
 */
static int srv_cli_find_free_locked(srv_t srv) {
  for (int i = 0; i < srv->cfg.max_clients; i++) {
    if (!srv->cli[i].in_use) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Find client slot by fd.
 * @param srv Server handle.
 * @param fd Client socket descriptor.
 * @return Slot index or -1 if not found. Lock must be held.
 */
static int srv_cli_find_by_fd_locked(srv_t srv, int fd) {
  for (int i = 0; i < srv->cfg.max_clients; i++) {
    if (srv->cli[i].in_use && srv->cli[i].fd == fd) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Add client, lock must be held.
 * @param srv Server handle.
 * @param fd New client socket descriptor.
 * @return Slot index or -1 if full.
 */
static int srv_cli_add_locked(srv_t srv, int fd) {
  int idx = srv_cli_find_free_locked(srv);
  if (idx < 0) {
    return -1;
  }
  srv->cli[idx].fd = fd;
  srv->cli[idx].in_use = true;
  srv->cli[idx].discarding = false;
  srv->cli[idx].rx_len = 0;
  srv->cli[idx].last_activity = xTaskGetTickCount();
  srv->diag.total_connections++;
  srv->diag.current_clients++;
  return idx;
}

/**
 * @brief Remove client, lock must be held.
 * @param srv Server handle.
 * @param idx Slot index.
 * @return fd to close or -1. Does NOT call on_disconnect, caller must do it
 * after unlocking.
 */
static int srv_cli_remove_locked(srv_t srv, int idx) {
  if (idx < 0 || idx >= srv->cfg.max_clients) {
    return -1;
  }
  if (!srv->cli[idx].in_use) {
    return -1;
  }
  int fd = srv->cli[idx].fd;
  srv->cli[idx].fd = -1;
  srv->cli[idx].in_use = false;
  srv->cli[idx].rx_len = 0;
  if (srv->diag.current_clients > 0) {
    srv->diag.current_clients--;
  }
  return fd;
}

/**
 * @brief Call on_disconnect callback outside lock.
 * @param srv Server handle.
 * @param fd Closed client fd, ignored if < 0.
 */
static void srv_cli_disconnect_notify(srv_t srv, int fd) {
  if (fd < 0) {
    return;
  }
  srv_on_client_cb_t cb = srv->cfg.on_disconnect;
  void *ctx = srv->cfg.user_ctx;
  if (cb) {
    cb(ctx, fd);
  }
}

/**
 * @brief Append data to client RX buffer.
 * @param srv Server handle.
 * @param idx Client slot index.
 * @param data Chunk from recv().
 * @param len Length of @p data.
 * @return 0 ok, line dropped is also 0, diag is updated internally. Lock must
 * be held.
 */
static int srv_cli_append_rx_locked(srv_t srv, int idx, const char *data,
                                    size_t len) {
  srv_cli_t *cli = &srv->cli[idx];
  srv->diag.rx_bytes += len;
  cli->last_activity = xTaskGetTickCount();
  size_t pos = 0;
  if (cli->discarding) {
    size_t i = 0;
    for (; i < len; i++) {
      if (data[i] == '\n') {
        break;
      }
    }
    if (i == len) {
      return 0;
    }
    cli->discarding = false;
    cli->rx_len = 0;
    pos = i + 1;
    if (pos >= len) {
      return 0;
    }
  }
  size_t rem_len = len - pos;
  const char *rem_data = data + pos;
  if (cli->rx_len + rem_len >= SRV_RX_BUF_SIZE) {
    cli->rx_len = 0;
    cli->discarding = true;
    srv->diag.dropped_lines++;
    ESP_LOGW(TAG, "rx overflow fd=%d, line dropped", cli->fd);
    for (size_t i = 0; i < rem_len; i++) {
      if (rem_data[i] == '\n') {
        cli->discarding = false;
        size_t after = rem_len - (i + 1);
        if (after > 0) {
          memcpy(cli->rx_buf, rem_data + i + 1, after);
          cli->rx_len = after;
        }
        break;
      }
    }
    return 0;
  }
  memcpy(cli->rx_buf + cli->rx_len, rem_data, rem_len);
  cli->rx_len += rem_len;
  return 0;
}

/**
 * @brief Extract one complete line, without `\r\n`.
 * @param srv Server handle.
 * @param idx Client slot index.
 * @param out Buffer for line, without `\r\n`, NUL-terminated.
 * @param out_size Size of @p out, must be >= 1.
 * @return -1 no complete line, -2 line too long and dropped,
 *         >=0 length of delivered line, 0 is valid empty line.
 * Caller must use return value as len, never strlen().
 * Lock must be held.
 */
static ssize_t srv_cli_extract_line_locked(srv_t srv, int idx, char *out,
                                           size_t out_size) {
  srv_cli_t *cli = &srv->cli[idx];
  for (size_t i = 0; i < cli->rx_len; i++) {
    if (cli->rx_buf[i] != '\n') {
      continue;
    }
    size_t line_len = i;
    if (line_len > 0 && cli->rx_buf[line_len - 1] == '\r') {
      line_len--;
    }
    size_t remain = cli->rx_len - (i + 1);
    if (line_len >= out_size) {
      memmove(cli->rx_buf, cli->rx_buf + i + 1, remain);
      cli->rx_len = remain;
      srv->diag.dropped_lines++;
      return -2;
    }
    memcpy(out, cli->rx_buf, line_len);
    out[line_len] = '\0';
    memmove(cli->rx_buf, cli->rx_buf + i + 1, remain);
    cli->rx_len = remain;
    srv->diag.rx_lines++;
    return (ssize_t)line_len;
  }
  return -1;
}

/**
 * @brief Close clients idle longer than srv_cfg_t.idle_timeout_ms.
 * @param srv Server handle.
 * @param now Current tick count from xTaskGetTickCount().
 */
static void srv_check_idle(srv_t srv, TickType_t now) {
  if (srv->cfg.idle_timeout_ms == 0) {
    return;
  }
  TickType_t timeout_ticks = pdMS_TO_TICKS(srv->cfg.idle_timeout_ms);
  int to_close[SRV_MAX_CLIENTS];
  int n_close = 0;
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return;
  }
  for (int i = 0; i < srv->cfg.max_clients; i++) {
    if (srv->cli[i].in_use) {
      if ((now - srv->cli[i].last_activity) > timeout_ticks) {
        int fd = srv_cli_remove_locked(srv, i);
        if (fd >= 0 && n_close < SRV_MAX_CLIENTS) {
          to_close[n_close++] = fd;
        }
      }
    }
  }
  xSemaphoreGive(srv->lock);
  for (int i = 0; i < n_close; i++) {
    ESP_LOGI(TAG, "idle timeout, close fd=%d", to_close[i]);
    close(to_close[i]);
    srv_cli_disconnect_notify(srv, to_close[i]);
  }
}

/* --- task internals --- */

/**
 * @brief Handle readable client sockets from select().
 * @param srv Server handle.
 * @param read_fds fd_set from select(), contains listen fd and client fds.
 */
static void srv_handle_cli(srv_t srv, fd_set *read_fds) {
  char tmp[SRV_RECV_TMP_SIZE];
  char line[SRV_RX_BUF_SIZE];
  int to_close[SRV_MAX_CLIENTS];
  int n_close = 0;
  for (int i = 0; i < srv->cfg.max_clients; i++) {
    int fd = -1;
    bool ready = false;
    if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }
    if (srv->cli[i].in_use && srv->cli[i].fd >= 0 &&
        FD_ISSET(srv->cli[i].fd, read_fds)) {
      fd = srv->cli[i].fd;
      ready = true;
    }
    xSemaphoreGive(srv->lock);
    if (!ready) {
      continue;
    }
    int r = recv(fd, tmp, sizeof(tmp), 0);
    if (r == 0) {
      if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idx = srv_cli_find_by_fd_locked(srv, fd);
        if (idx >= 0) {
          int cfd = srv_cli_remove_locked(srv, idx);
          if (cfd >= 0 && n_close < SRV_MAX_CLIENTS) {
            to_close[n_close++] = cfd;
          }
        }
        xSemaphoreGive(srv->lock);
      }
      continue;
    }
    if (r < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR) {
        continue;
      }
      ESP_LOGW(TAG, "recv fd=%d errno=%d, disconnect", fd, errno);
      if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idx = srv_cli_find_by_fd_locked(srv, fd);
        if (idx >= 0) {
          int cfd = srv_cli_remove_locked(srv, idx);
          if (cfd >= 0 && n_close < SRV_MAX_CLIENTS) {
            to_close[n_close++] = cfd;
          }
        }
        xSemaphoreGive(srv->lock);
      }
      continue;
    }
    if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }
    int idx = srv_cli_find_by_fd_locked(srv, fd);
    if (idx < 0) {
      xSemaphoreGive(srv->lock);
      continue;
    }
    srv_cli_append_rx_locked(srv, idx, tmp, (size_t)r);
    xSemaphoreGive(srv->lock);
    while (1) {
      ssize_t r = -1;
      if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        break;
      }
      idx = srv_cli_find_by_fd_locked(srv, fd);
      if (idx < 0) {
        xSemaphoreGive(srv->lock);
        break;
      }
      r = srv_cli_extract_line_locked(srv, idx, line, sizeof(line));
      xSemaphoreGive(srv->lock);
      if (r == -1) {
        break;
      }
      if (r == -2) {
        continue;
      }
      if (srv->cfg.on_data) {
        srv->cfg.on_data(srv->cfg.user_ctx, fd, line, (size_t)r);
      } else {
        if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
          srv->diag.dropped_lines++;
          xSemaphoreGive(srv->lock);
        }
      }
    }
  }
  for (int i = 0; i < n_close; i++) {
    close(to_close[i]);
    srv_cli_disconnect_notify(srv, to_close[i]);
  }
}

/**
 * @brief srv task, owns listen socket and select() loop.
 * @param pvParameters Server handle, cast to srv_t.
 */
static void srv_task(void *pvParameters) {
  srv_t srv = (srv_t)pvParameters;
  int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_fd < 0) {
    ESP_LOGE(TAG, "socket failed errno=%d", errno);
    srv->start_ok = false;
    xSemaphoreGive(srv->started_sem);
    srv->running = false;
    xSemaphoreGive(srv->stopped_sem);
    vTaskDelete(NULL);
    return;
  }
  srv->listen_fd = listen_fd;
  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in srv_addr;
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  srv_addr.sin_port = htons(srv->cfg.port);
  if (bind(listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
    ESP_LOGE(TAG, "bind port %u failed errno=%d", srv->cfg.port, errno);
    close(listen_fd);
    srv->listen_fd = -1;
    srv->start_ok = false;
    xSemaphoreGive(srv->started_sem);
    srv->running = false;
    xSemaphoreGive(srv->stopped_sem);
    vTaskDelete(NULL);
    return;
  }
  if (listen(listen_fd, srv->cfg.backlog) != 0) {
    ESP_LOGE(TAG, "listen failed errno=%d", errno);
    close(listen_fd);
    srv->listen_fd = -1;
    srv->start_ok = false;
    xSemaphoreGive(srv->started_sem);
    srv->running = false;
    xSemaphoreGive(srv->stopped_sem);
    vTaskDelete(NULL);
    return;
  }
  int flags = fcntl(listen_fd, F_GETFL, 0);
  fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);
  ESP_LOGI(TAG, "listening on port %u, max %u", srv->cfg.port,
           srv->cfg.max_clients);
  srv->start_ok = true;
  xSemaphoreGive(srv->started_sem);
  while (srv->running) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listen_fd, &read_fds);
    int max_fd = listen_fd;
    if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
      for (int i = 0; i < srv->cfg.max_clients; i++) {
        if (srv->cli[i].in_use && srv->cli[i].fd >= 0 &&
            srv->cli[i].fd < FD_SETSIZE) {
          FD_SET(srv->cli[i].fd, &read_fds);
          if (srv->cli[i].fd > max_fd) {
            max_fd = srv->cli[i].fd;
          }
        }
      }
      xSemaphoreGive(srv->lock);
    }
    if (max_fd >= FD_SETSIZE) {
      ESP_LOGE(TAG, "max_fd %d >= FD_SETSIZE", max_fd);
      break;
    }
    struct timeval timeout = {
        .tv_sec = SRV_SELECT_TIMEOUT_MS / 1000,
        .tv_usec = (SRV_SELECT_TIMEOUT_MS % 1000) * 1000,
    };
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      ESP_LOGE(TAG, "select failed errno=%d", errno);
      break;
    }
    if (ret == 0) {
      srv_check_idle(srv, xTaskGetTickCount());
      continue;
    }
    if (FD_ISSET(listen_fd, &read_fds)) {
      while (1) {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int new_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &len);
        if (new_fd < 0) {
          if (errno == EWOULDBLOCK || errno == EAGAIN) {
            break;
          }
          if (errno == EMFILE || errno == ENFILE) {
            ESP_LOGW(TAG, "accept EMFILE, no fd");
            break;
          }
          if (errno == ECONNABORTED) {
            continue;
          }
          ESP_LOGE(TAG, "accept failed errno=%d", errno);
          break;
        }
        int cflags = fcntl(new_fd, F_GETFL, 0);
        fcntl(new_fd, F_SETFL, cflags | O_NONBLOCK);
        int idx = -1;
        if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
          idx = srv_cli_add_locked(srv, new_fd);
          xSemaphoreGive(srv->lock);
        }
        if (idx < 0) {
          ESP_LOGW(TAG, "max clients, close fd=%d", new_fd);
          close(new_fd);
          continue;
        }
        ESP_LOGI(TAG, "new client fd=%d ip=%s", new_fd,
                 inet_ntoa(cli_addr.sin_addr));
        if (srv->cfg.on_connect) {
          srv->cfg.on_connect(srv->cfg.user_ctx, new_fd);
        }
      }
    }
    srv_handle_cli(srv, &read_fds);
    srv_check_idle(srv, xTaskGetTickCount());
  }
  ESP_LOGI(TAG, "task cleanup");
  close(listen_fd);
  srv->listen_fd = -1;
  int to_close[SRV_MAX_CLIENTS];
  int n_close = 0;
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) == pdTRUE) {
    for (int i = 0; i < srv->cfg.max_clients; i++) {
      int fd = srv_cli_remove_locked(srv, i);
      if (fd >= 0 && n_close < SRV_MAX_CLIENTS) {
        to_close[n_close++] = fd;
      }
    }
    xSemaphoreGive(srv->lock);
  }
  for (int i = 0; i < n_close; i++) {
    close(to_close[i]);
    srv_cli_disconnect_notify(srv, to_close[i]);
  }
  srv->running = false;
  srv->handle = NULL;
  xSemaphoreGive(srv->stopped_sem);
  vTaskDelete(NULL);
}

/* --- public API --- */

srv_err_t srv_init(srv_handle_storage_t *storage, const srv_cfg_t *cfg,
                   srv_t *out) {
  if (!storage || !cfg || !out) {
    return SRV_ERR_INVALID_ARG;
  }
  if (cfg->port == 0) {
    ESP_LOGE(TAG, "invalid port 0");
    return SRV_ERR_INVALID_ARG;
  }
  if (cfg->max_clients == 0 || cfg->max_clients > SRV_MAX_CLIENTS) {
    ESP_LOGE(TAG, "invalid max_clients %u", cfg->max_clients);
    return SRV_ERR_INVALID_ARG;
  }
  if (cfg->stack_size != 0 && cfg->stack_size != SRV_STACK_SIZE) {
    ESP_LOGE(TAG, "stack_size must be 0 or %d", SRV_STACK_SIZE);
    return SRV_ERR_INVALID_ARG;
  }
  if (cfg->rx_buf_size != 0 && cfg->rx_buf_size != SRV_RX_BUF_SIZE) {
    ESP_LOGE(TAG, "rx_buf_size must be 0 or %d", SRV_RX_BUF_SIZE);
    return SRV_ERR_INVALID_ARG;
  }
  struct srv_s *srv = (struct srv_s *)storage;
  memset(srv, 0, sizeof(*srv));
  srv->cfg = *cfg;
  if (srv->cfg.backlog <= 0) {
    srv->cfg.backlog = SRV_DEFAULT_BACKLOG;
  }
  if (srv->cfg.stack_size == 0) {
    srv->cfg.stack_size = SRV_STACK_SIZE;
  }
  if (srv->cfg.rx_buf_size == 0) {
    srv->cfg.rx_buf_size = SRV_RX_BUF_SIZE;
  }
  if (srv->cfg.priority == 0) {
    srv->cfg.priority = 5;
  }
  if (srv->cfg.core_id > 1) {
    return SRV_ERR_INVALID_ARG;
  }
  srv->listen_fd = -1;
  srv->running = false;
  srv->handle = NULL;
  for (int i = 0; i < SRV_MAX_CLIENTS; i++) {
    srv->cli[i].fd = -1;
    srv->cli[i].in_use = false;
    srv->cli[i].rx_len = 0;
  }
  srv->lock = xSemaphoreCreateMutexStatic(&srv->lock_buf);
  srv->stopped_sem = xSemaphoreCreateBinaryStatic(&srv->stopped_buf);
  srv->started_sem = xSemaphoreCreateBinaryStatic(&srv->started_buf);
  if (!srv->lock || !srv->stopped_sem || !srv->started_sem) {
    return SRV_ERR_FAIL;
  }
  srv->start_ok = false;
  *out = srv;
  return SRV_OK;
}

srv_err_t srv_deinit(srv_t srv) {
  if (!srv) {
    return SRV_ERR_INVALID_ARG;
  }
  if (srv->running) {
    srv_stop(srv);
  } else if (srv->handle != NULL) {
    xSemaphoreTake(srv->stopped_sem, pdMS_TO_TICKS(SRV_STOP_TIMEOUT_MS));
    srv->handle = NULL;
  }
  srv->lock = NULL;
  srv->stopped_sem = NULL;
  srv->started_sem = NULL;
  srv->handle = NULL;
  srv->listen_fd = -1;
  srv->running = false;
  srv->start_ok = false;
  return SRV_OK;
}

srv_err_t srv_start(srv_t srv) {
  if (!srv) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock || !srv->started_sem || !srv->stopped_sem) {
    return SRV_ERR_INVALID_ARG;
  }
  if (srv->running) {
    return SRV_ERR_ALREADY_STARTED;
  }
  xSemaphoreTake(srv->stopped_sem, 0);
  xSemaphoreTake(srv->started_sem, 0);
  srv->start_ok = false;
  srv->running = true;
  srv->handle = xTaskCreateStaticPinnedToCore(
      srv_task, "srv", SRV_STACK_DEPTH, srv, srv->cfg.priority, srv->stack,
      &srv->tcb, srv->cfg.core_id);
  if (!srv->handle) {
    srv->running = false;
    ESP_LOGE(TAG, "xTaskCreateStatic failed");
    return SRV_ERR_FAIL;
  }
  if (xSemaphoreTake(srv->started_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGE(TAG, "srv task did not report start");
    srv->running = false;
    if (srv->listen_fd != -1) {
      shutdown(srv->listen_fd, SHUT_RDWR);
    }
    xSemaphoreTake(srv->stopped_sem, pdMS_TO_TICKS(1000));
    srv->handle = NULL;
    return SRV_ERR_FAIL;
  }
  if (!srv->start_ok) {
    xSemaphoreTake(srv->stopped_sem, pdMS_TO_TICKS(1000));
    srv->running = false;
    srv->handle = NULL;
    return SRV_ERR_FAIL;
  }
  return SRV_OK;
}

srv_err_t srv_stop(srv_t srv) {
  if (!srv) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock || !srv->started_sem || !srv->stopped_sem) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->running) {
    return SRV_ERR_NOT_STARTED;
  }
  srv->running = false;
  if (srv->listen_fd != -1) {
    shutdown(srv->listen_fd, SHUT_RDWR);
  }
  if (xSemaphoreTake(srv->stopped_sem, pdMS_TO_TICKS(SRV_STOP_TIMEOUT_MS)) !=
      pdTRUE) {
    ESP_LOGE(TAG, "srv task did not exit in %d ms", SRV_STOP_TIMEOUT_MS);
    return SRV_ERR_FAIL;
  }
  srv->handle = NULL;
  return SRV_OK;
}

bool srv_is_running(srv_t srv) {
  if (!srv) {
    return false;
  }
  return srv->running;
}

srv_err_t srv_send_to(srv_t srv, int fd, const char *data, size_t len) {
  if (!srv || !data || len == 0) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock || !srv->started_sem || !srv->stopped_sem) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->running) {
    return SRV_ERR_NOT_STARTED;
  }
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return SRV_ERR_FAIL;
  }
  int idx = srv_cli_find_by_fd_locked(srv, fd);
  if (idx < 0) {
    xSemaphoreGive(srv->lock);
    return SRV_ERR_NO_CLIENT;
  }
  int send_fd = srv->cli[idx].fd;
  int n = send(send_fd, data, len, 0);
  srv_err_t ret = SRV_OK;
  int fd_to_close = -1;
  if (n < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      ret = SRV_ERR_WOULD_BLOCK;
    } else {
      ESP_LOGW(TAG, "send fd=%d errno=%d, disconnect", send_fd, errno);
      fd_to_close = srv_cli_remove_locked(srv, idx);
      ret = SRV_ERR_FAIL;
    }
  } else {
    srv->diag.tx_bytes += (uint32_t)n;
    srv->cli[idx].last_activity = xTaskGetTickCount();
    if ((size_t)n < len) {
      ret = SRV_ERR_WOULD_BLOCK;
    }
  }
  xSemaphoreGive(srv->lock);
  if (fd_to_close >= 0) {
    close(fd_to_close);
    srv_cli_disconnect_notify(srv, fd_to_close);
  }
  return ret;
}

srv_err_t srv_send_all(srv_t srv, const char *data, size_t len) {
  if (!srv || !data || len == 0) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock || !srv->started_sem || !srv->stopped_sem) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->running) {
    return SRV_ERR_NOT_STARTED;
  }
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return SRV_ERR_FAIL;
  }
  int to_close[SRV_MAX_CLIENTS];
  int n_close = 0;
  int sent_cnt = 0;
  for (int i = 0; i < srv->cfg.max_clients; i++) {
    if (!srv->cli[i].in_use) {
      continue;
    }
    int send_fd = srv->cli[i].fd;
    int n = send(send_fd, data, len, 0);
    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        continue;
      }
      int fd = srv_cli_remove_locked(srv, i);
      if (fd >= 0 && n_close < SRV_MAX_CLIENTS) {
        to_close[n_close++] = fd;
      }
    } else {
      srv->diag.tx_bytes += (uint32_t)n;
      srv->cli[i].last_activity = xTaskGetTickCount();
      sent_cnt++;
    }
  }
  xSemaphoreGive(srv->lock);
  for (int i = 0; i < n_close; i++) {
    close(to_close[i]);
    srv_cli_disconnect_notify(srv, to_close[i]);
  }
  if (sent_cnt == 0 && n_close > 0) {
    return SRV_ERR_FAIL;
  }
  return SRV_OK;
}

srv_err_t srv_get_diag(srv_t srv, srv_diag_t *out) {
  if (!srv || !out) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock) {
    return SRV_ERR_INVALID_ARG;
  }
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return SRV_ERR_FAIL;
  }
  *out = srv->diag;
  xSemaphoreGive(srv->lock);
  return SRV_OK;
}

srv_err_t srv_reset_diag(srv_t srv) {
  if (!srv) {
    return SRV_ERR_INVALID_ARG;
  }
  if (!srv->lock || !srv->started_sem || !srv->stopped_sem) {
    return SRV_ERR_INVALID_ARG;
  }
  if (xSemaphoreTake(srv->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
    return SRV_ERR_FAIL;
  }
  uint32_t cur = srv->diag.current_clients;
  memset(&srv->diag, 0, sizeof(srv->diag));
  srv->diag.current_clients = cur;
  xSemaphoreGive(srv->lock);
  return SRV_OK;
}

const char *srv_err_to_str(srv_err_t err) {
  switch (err) {
  case SRV_OK:
    return "OK";
  case SRV_ERR_INVALID_ARG:
    return "INVALID_ARG";
  case SRV_ERR_FAIL:
    return "FAIL";
  case SRV_ERR_NO_MEM:
    return "NO_MEM";
  case SRV_ERR_ALREADY_STARTED:
    return "ALREADY_STARTED";
  case SRV_ERR_NOT_STARTED:
    return "NOT_STARTED";
  case SRV_ERR_NO_CLIENT:
    return "NO_CLIENT";
  case SRV_ERR_WOULD_BLOCK:
    return "WOULD_BLOCK";
  default:
    return "UNKNOWN";
  }
}