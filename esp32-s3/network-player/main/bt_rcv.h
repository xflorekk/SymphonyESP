#ifndef BT_RCV_H
#define BT_RCV_H

typedef enum {
  BT_RCV_CMD_OK = 0,
  BT_RCV_CMD_SHUTDOWN,
} bt_rcv_cmd_type_t;

typedef struct {
  bt_rcv_cmd_type_t type;
} bt_rcv_cmd_t;

int bt_rcv_init(void);
int bt_rcv_start(void);
int bt_rcv_stop(void);
int bt_rcv_send_cmd(bt_rcv_cmd_t *cmd);

#endif