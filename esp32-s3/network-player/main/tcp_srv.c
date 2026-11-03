#include "tcp_srv.h"
#include "app.h"
#include "json.h"
#include "tcp_cli.h"
#include <errno.h>
#include <esp_log.h>
#include <fcntl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <string.h>

static const char *TAG = "TCP_SRV";

#define TCP_SRV_STACK_SIZE 8192
#define TCP_SRV_STACK_DEPTH (TCP_SRV_STACK_SIZE / sizeof(StackType_t))

typedef struct {
  StackType_t stack[TCP_SRV_STACK_DEPTH];
  StaticTask_t tcb;
  TaskHandle_t handle;
  volatile bool running;
  int listen_fd;
} tcp_srv_ctx_t;

static tcp_srv_ctx_t s_ctx;

static int tcp_srv_handle_clients(fd_set *read_fds) {
  tcp_cli_t *cli = tcp_cli_get_next(NULL);
  while (cli) {
    tcp_cli_t *next = tcp_cli_get_next(cli);

    if (FD_ISSET(cli->fd, read_fds)) {
      ESP_LOGI(TAG, "fd %d ISSET", cli->fd);
      char tmp[256];
      int r = recv(cli->fd, tmp, sizeof(tmp), 0);
      ESP_LOGI(TAG, "recv fd=%d r=%d", cli->fd, r);

      if (r <= 0) {
        tcp_cli_remove(cli);
      } else {
        if (tcp_cli_append_rx(cli, tmp, r) != 0) {
          tcp_cli_remove(cli);
        } else {
          char line[TCP_RX_BUF_SIZE];
          while (tcp_cli_extract_line(cli, line, sizeof(line)) > 0) {
            ESP_LOGI(TAG, "RX fd=%d: %s", cli->fd, line);

            app_cmd_t cmd;
            if (json_parse(line, &cmd) != 0) {
              char err[128];
              json_build_error(err, sizeof(err), "invalid json");
              send(cli->fd, err, strlen(err), 0);
              continue;
            }
            cmd.src_fd = cli->fd;
            if (app_send_cmd(&cmd) != 0) {
              char err[128];
              json_build_error(err, sizeof(err), "queue full");
              send(cli->fd, err, strlen(err), 0);
            }
          }
        }
      }
    } else {
      ESP_LOGD(TAG, "fd %d NOT ready", cli->fd);
    }

    cli = next;
  }
  return 0;
}

static void tcp_srv_task(void *pvParameters) {
  // 1. INIT
  // socket()
  // bind()
  // listen()

  s_ctx.listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (s_ctx.listen_fd < 0) {
    ESP_LOGE(TAG, "socket() failed: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(s_ctx.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in srv_addr;
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  srv_addr.sin_port = htons(TCP_SRV_PORT);

  int err =
      bind(s_ctx.listen_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "bind() failed: errno %d", errno);
    close(s_ctx.listen_fd);
    vTaskDelete(NULL);
    return;
  }

  err = listen(s_ctx.listen_fd, 5);
  if (err != 0) {
    ESP_LOGE(TAG, "listen() failed: %d", errno);
    close(s_ctx.listen_fd);
    vTaskDelete(NULL);
    return;
  }

  // dopiero po udanym listen
  int flags = fcntl(s_ctx.listen_fd, F_GETFL, 0);
  fcntl(s_ctx.listen_fd, F_SETFL, flags | O_NONBLOCK);

  ESP_LOGI(TAG, "listening on port %d", TCP_SRV_PORT);

  while (s_ctx.running) {
    // 2. LISTENING CONFIG
    // building descriptor list fd_set
    // adding listening socket to the list
    // adding to list active clients

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(s_ctx.listen_fd, &read_fds);
    int max_fd = s_ctx.listen_fd;
    tcp_cli_fill_fdset(&read_fds, &max_fd);

    if (max_fd >= FD_SETSIZE) {
      ESP_LOGE(TAG, "max_fd %d >= FD_SETSIZE %d !", max_fd, FD_SETSIZE);
      max_fd = FD_SETSIZE - 1;
    }
    // 3. WAITING FOR ACTION -> select()
    // select() - blocking function

    // what's the timeout purpose? what's the mechanism here?
    // timeout is to block on select for 1 second, NULL means block indefinetely
    struct timeval timeout = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    int ret = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
    // ESP_LOGI(TAG, "select ret=%d max=%d", ret, max_fd);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      ESP_LOGE(TAG, "select() failed: errno %d", errno);
      break;
    }
    if (ret == 0) {
      continue; // timeout
    }

    // 4. NEW CLIENT
    // if new: accept(), tcp_cli_create(),
    // to new client send config data

    if (FD_ISSET(s_ctx.listen_fd, &read_fds)) {
      while (1) {
        struct sockaddr_in cli_addr;
        socklen_t len = sizeof(cli_addr);
        int new_fd =
            accept(s_ctx.listen_fd, (struct sockaddr *)&cli_addr, &len);
        if (new_fd < 0) {
          if (errno == EWOULDBLOCK || errno == EAGAIN)
            break; // koniec kolejki
          ESP_LOGE(TAG, "accept failed %d", errno);
          break;
        }
        tcp_cli_t *cli = tcp_cli_create(new_fd, &cli_addr);
        if (!cli) {
          close(new_fd);
          continue;
        }
        const char *welcome = "{\"type\":\"welcome\",\"clients\":1}\n";
        send(new_fd, welcome, strlen(welcome), 0);
      }
    }

    // 5. PARSE, SEND CMD
    // clients registered
    // recv -> action, get data, if 0, <0 tcp_cli_remove()
    // if data: parse, send app_send_cmd();
    tcp_srv_handle_clients(&read_fds);
  }

  ESP_LOGI(TAG, "task cleanup");
  close(s_ctx.listen_fd);
  s_ctx.listen_fd = -1;

  tcp_cli_t *cli = NULL;
  while ((cli = tcp_cli_get_next(NULL)) != NULL) {
    tcp_cli_remove(cli);
  }

  s_ctx.handle = NULL;
  s_ctx.running = false;
  vTaskDelete(NULL);
}

int tcp_srv_init(void) {
  s_ctx.handle = NULL;
  s_ctx.running = false;
  s_ctx.listen_fd = -1;
  tcp_cli_pool_init();
  return 0;
}

int tcp_srv_start(void) {
  if (s_ctx.running) {
    return -1;
  }
  s_ctx.running = true;
  s_ctx.handle = xTaskCreateStaticPinnedToCore(tcp_srv_task, "tcp_srv",
                                               TCP_SRV_STACK_DEPTH, NULL, 4,
                                               s_ctx.stack, &s_ctx.tcb, 0);

  if (!s_ctx.handle) {
    s_ctx.running = false;
    return -1;
  }
  return 0;
}

int tcp_srv_stop(void) {
  if (!s_ctx.running) {
    return -1;
  }
  s_ctx.running = false;
  if (s_ctx.listen_fd != -1) {
    shutdown(s_ctx.listen_fd, SHUT_RDWR);
  }

  return 0;
}

int tcp_srv_broadcast(const char *json) {
  if (!json) {
    return -1;
  }
  size_t len = strlen(json);
  int cnt = 0;

  tcp_cli_t *cli = tcp_cli_get_next(NULL);

  while (cli) {
    tcp_cli_t *next = tcp_cli_get_next(cli);
    int n = send(cli->fd, json, len, 0);
    ESP_LOGI(TAG, "bcast to fd=%d n=%d err=%d", cli->fd, n, n < 0 ? errno : 0);
    if (n >= 0) {
      cnt++;
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
      tcp_cli_remove(cli);
    }
    cli = next;
  }
  return cnt;
}

int tcp_srv_unicast(int fd, const char *json) {
  if (fd < 0 || !json) {
    return -1;
  }

  tcp_cli_t *cli = tcp_cli_find_by_fd(fd);
  if (!cli) {
    return -1;
  }

  int n = send(cli->fd, json, strlen(json), 0);
  if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
    tcp_cli_remove(cli);
  }
  return n;
}

int tcp_serv_send_error(int fd, const char *json) {
  return tcp_srv_unicast(fd, json);
}