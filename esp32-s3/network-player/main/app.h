#ifndef APP_H
#define APP_H

#include "app_types.h"
#include <stdbool.h>

typedef struct {
  app_cmd_type_t type;
  int src_fd;
  int value;
  char data[256];
} app_cmd_t;

typedef struct {
  int volume;
  bool is_muted;
  bool is_playing;
  char url[256];
  char title[128];
  char artist[128];
} app_state_t;

typedef struct app_obj *app_handle_t;

int app_init(void);
int app_send_cmd(const app_cmd_t *cmd);
int app_get_snapshot(app_state_t *out);

#endif