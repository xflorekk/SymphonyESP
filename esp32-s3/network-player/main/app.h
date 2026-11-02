#ifndef MSYS_H
#define MSYS_H

typedef enum {
  APP_CMD_OK = 0,
} app_cmd_type_t;

typedef struct {
  app_cmd_type_t type;
} app_cmd_t;

int app_init(void);
int app_send_cmd(app_cmd_t *cmd);

#endif