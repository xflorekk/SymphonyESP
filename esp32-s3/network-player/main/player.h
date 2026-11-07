#ifndef PLAYER_H
#define PLAYER_H

typedef enum {
  PLAYER_CMD_OK = 0,
} player_cmd_type_t;

typedef struct {
  player_cmd_type_t type;
} player_cmd_t;

int player_init(void);
int player_send_cmd(const player_cmd_t *cmd);

#endif