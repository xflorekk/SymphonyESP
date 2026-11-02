#ifndef TCP_CLI_H
#define TCP_CLI_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lwip/sockets.h>
#include <stdbool.h>
#include <stdint.h>

/* TODO:
    Define the format client,
    could use OOP in C, but we want to avoid dynamic allocation
    structures for each client out of pool (max cli num)
    should be assigned statically
    What data we keep?
*/

#define TCP_MAX_CLIENTS 8
#define TCP_RX_BUF_SIZE 1024

typedef enum {
  TCP_CLI_STATE_FREE = 0,
  TCP_CLI_STATE_CONNECTED,
} tcp_cli_state_t;

typedef struct {
  int fd;
  uint16_t rx_len;
  char rx_buf[TCP_RX_BUF_SIZE];
  struct sockaddr_in addr;
  tcp_cli_state_t state;
  TickType_t last_activity;
} tcp_cli_t;

int tcp_cli_pool_init(void);
tcp_cli_t *tcp_cli_create(int fd, struct sockaddr_in *addr);
int tcp_cli_remove(tcp_cli_t *cli);
int tcp_cli_remove_by_fd(int fd);

// lookup / select
tcp_cli_t *tcp_cli_find_by_fd(int fd);
tcp_cli_t *tcp_cli_get_next(tcp_cli_t *prev_cli);
int tcp_cli_fill_fdset(fd_set *set, int *max_fd);
int tcp_cli_count(void);

// ---JSON Lines---
int tcp_cli_append_rx(tcp_cli_t *cli, const char *data, int len);
int tcp_cli_extract_line(tcp_cli_t *cli, char *out_line, size_t out_size);

#endif