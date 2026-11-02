#include "tcp_cli.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "TCP_CLI";

static int s_cli_count;
static tcp_cli_t s_clients[TCP_MAX_CLIENTS];
static SemaphoreHandle_t s_lock = NULL;

int tcp_cli_pool_init(void) {
  if (s_lock) {
    return 0;
  }
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) {
    return -1;
  }

  for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
    s_clients[i].fd = -1;
    s_clients[i].state = TCP_CLI_STATE_FREE;
    s_clients[i].rx_len = 0;
  }
  s_cli_count = 0;
  return 0;
}

tcp_cli_t *tcp_cli_create(int fd, struct sockaddr_in *addr) {
  if (fd < 0) {
    return NULL;
  }
  tcp_cli_t *new_cli = NULL;
  xSemaphoreTake(s_lock, pdMS_TO_TICKS(100));

  for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
    if (s_clients[i].state == TCP_CLI_STATE_FREE) {
      s_clients[i].fd = fd;
      s_clients[i].state = TCP_CLI_STATE_CONNECTED;
      s_clients[i].rx_len = 0;
      s_clients[i].last_activity = xTaskGetTickCount();
      if (addr) {
        s_clients[i].addr = *addr;
      }
      new_cli = &s_clients[i];
      s_cli_count++;
      ESP_LOGI(TAG, "new client fd=%d slot=%d ip=%s", fd, i,
               inet_ntoa(addr->sin_addr));
      break;
    }
  }

  xSemaphoreGive(s_lock);
  return new_cli;
}

int tcp_cli_remove(tcp_cli_t *cli) {
  if (!cli) {
    return -1;
  }
  if (cli < s_clients || cli >= s_clients + TCP_MAX_CLIENTS) {
    return -1;
  }

  int fd_to_close = -1;
  xSemaphoreTake(s_lock, pdMS_TO_TICKS(100));
  if (cli->state != TCP_CLI_STATE_FREE) {
    fd_to_close = cli->fd;
    cli->fd = -1;
    cli->state = TCP_CLI_STATE_FREE;
    cli->rx_len = 0;
  }
  xSemaphoreGive(s_lock);

  if (fd_to_close != -1) {
    close(fd_to_close);
    s_cli_count--;
    ESP_LOGI(TAG, "client removed fd=%d", fd_to_close);
    return 0;
  }
  return -1;
}

int tcp_cli_remove_by_fd(int fd) {
  tcp_cli_t *cli = tcp_cli_find_by_fd(fd);
  return cli ? tcp_cli_remove(cli) : -1; // how does it work?
}

tcp_cli_t *tcp_cli_get_next(tcp_cli_t *prev) {
  int start = 0;
  if (prev) {
    start = (prev - s_clients) + 1;
  }

  xSemaphoreTake(s_lock, pdMS_TO_TICKS(100));
  tcp_cli_t *next = NULL;
  for (int i = start; i < TCP_MAX_CLIENTS; i++) {

    if (s_clients[i].state != TCP_CLI_STATE_FREE) {
      next = &s_clients[i];
      break;
    }
  }
  xSemaphoreGive(s_lock);
  return next;
}

int tcp_cli_fill_fdset(fd_set *set, int *max_fd) {
  int cnt = 0;
  if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE)
    return 0;
  for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
    if (s_clients[i].state != TCP_CLI_STATE_FREE) {
      if (s_clients[i].fd >= 0 && s_clients[i].fd < FD_SETSIZE) {
        FD_SET(s_clients[i].fd, set);
        if (s_clients[i].fd > *max_fd)
          *max_fd = s_clients[i].fd;
        cnt++;
      }
    }
  }
  xSemaphoreGive(s_lock);
  return cnt;
}

int tcp_cli_count(void) { return s_cli_count; }

// --- JSON Lines ---
int tcp_cli_append_rx(tcp_cli_t *cli, const char *data, int len) {
  if (!cli || !data || len <= 0) {
    return -1;
  }
  if (cli->rx_len + len >= TCP_RX_BUF_SIZE) {
    ESP_LOGW(TAG, "rx overflow fd=%d, dropping", cli->fd);
    return -1;
  }
  memcpy(cli->rx_buf + cli->rx_len, data, len);
  cli->rx_len += len;
  cli->last_activity = xTaskGetTickCount();
  return 0;
}

int tcp_cli_extract_line(tcp_cli_t *cli, char *out_line, size_t out_size) {
  if (!cli || !out_line) {
    return 0;
  }

  for (int i = 0; i < cli->rx_len; i++) {
    if (cli->rx_buf[i] == '\n') {
      int line_len = i;
      if (line_len > 0 && cli->rx_buf[line_len - 1] == '\r') {
        line_len--;
      }
      if (line_len >= (int)out_size) {
        line_len = out_size - 1;
      }
      memcpy(out_line, cli->rx_buf, line_len);
      out_line[line_len] = '\0';

      int remain = cli->rx_len - (i + 1);
      memmove(cli->rx_buf, cli->rx_buf + i + 1, remain);
      cli->rx_len = remain;
      return line_len;
    }
  }
  return 0;
}