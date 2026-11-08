#ifndef NET_SRV_H
#define NET_SRV_H

#include "app_types.h"

typedef enum {
  NET_SRV_CMD_OK = 0,
  NET_SRV_CMD_CONFIG,
  NET_SRV_CMD_PLAY,
  NET_SRV_CMD_STOP,
  NET_SRV_CMD_SHUTDOWN,
} net_srv_cmd_type_t;

typedef struct {
  net_srv_cmd_type_t type;
  char srv_ip[32];
  uint16_t srv_port;
  player_format_t format;
  uint32_t rate;
} net_srv_cmd_t;

int net_srv_init(void);
int net_srv_start(void);
int net_srv_stop(void);
int net_srv_send_cmd(net_srv_cmd_t *cmd);

#endif