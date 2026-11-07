#ifndef NET_SRV_H
#define NET_SRV_H

typedef enum {
  NET_SRV_CMD_OK = 0,
  NET_SRV_CMD_SHUTDOWN,
} net_srv_cmd_type_t;

typedef struct {
  net_srv_cmd_type_t type;
} net_srv_cmd_t;

int net_srv_init(void);
int net_srv_start(void);
int net_srv_stop(void);
int net_srv_send_cmd(net_srv_cmd_t *cmd);

#endif