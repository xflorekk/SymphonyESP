#ifndef APP_H
#define APP_H

#include "app_types.h"
#include <stdbool.h>

typedef struct app_obj *app_handle_t;

int app_init(void);
int app_send_cmd(const app_cmd_t *cmd);
int app_get_snapshot(app_state_t *out);

#endif