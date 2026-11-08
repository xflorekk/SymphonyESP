#ifndef NET_RDO_H
#define NET_RDO_H

typedef enum {
  NET_RDO_CMD_OK = 0,
  NET_RDO_CMD_CONFIG,
  NET_RDO_CMD_PLAY,
  NET_RDO_CMD_STOP,
  NET_RDO_CMD_SHUTDOWN,
} net_rdo_cmd_type_t;

typedef struct {
  net_rdo_cmd_type_t type;
  char url[256];
} net_rdo_cmd_t;

int net_rdo_init(void);
int net_rdo_start(void);
int net_rdo_stop(void);
int net_rdo_send_cmd(net_rdo_cmd_t *cmd);

#endif