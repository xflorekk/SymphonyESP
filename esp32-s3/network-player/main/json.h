#ifndef JSON_H
#define JSON_H

/* TODO:
    JSON parsing
*/
#include "app.h"
#include "app_types.h"
#include <stddef.h>

int json_parse(const char *line, app_cmd_t *out);

// state send after logging
// error if fail in any process
// send broadcast about changed information
int json_build_state(char *out, size_t out_size, const app_state_t *st);
int json_build_error(char *out, size_t out_size, const char *msg);
int json_build_broadcast(char *out, size_t out_size, const app_state_t *st,
                         app_cmd_type_t changed);

#endif