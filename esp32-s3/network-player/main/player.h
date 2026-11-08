#ifndef PLAYER_H
#define PLAYER_H

#include "app_types.h"
#include <stdint.h>

typedef enum {
  PLAYER_CMD_OK = 0,
  PLAYER_CMD_CONFIG,
  PLAYER_CMD_PLAY,
  PLAYER_CMD_STOP,
  PLAYER_CMD_SHUTDOWN,
} player_cmd_type_t;

typedef struct {
  player_cmd_type_t type;
  player_format_t format;
  uint32_t rate;
} player_cmd_t;

int player_init(void);
int player_send_cmd(const player_cmd_t *cmd);

#endif