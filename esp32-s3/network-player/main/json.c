#include "json.h"
#include <stdio.h>
#include <string.h>

static int count_substr(const char *line, const char *needle) {
  int cnt = 0;
  const char *p = line;
  size_t nlen = strlen(needle);
  while ((p = strstr(p, needle)) != NULL) {
    cnt++;
    p += nlen;
    if (cnt > 1) {
      break;
    }
  }

  return cnt;
}

static int extract_int(const char *line, const char *key, int *out) {
  const char *p = strstr(line, key);
  if (!p) {
    return -1;
  }
  p = strchr(p, ':');
  if (!p) {
    return -1;
  }
  return sscanf(p + 1, "%d", out) == 1 ? 0 : -1;
}

static int extract_string(const char *line, const char *key, char *out,
                          size_t out_size) {
  const char *p = strstr(line, key);
  if (!p) {
    return -1;
  }
  p = strchr(p, ':');
  if (!p) {
    return -1;
  }
  p = strchr(p, '"');
  if (!p) {
    return -1;
  }
  p++;
  const char *e = strchr(p, '"');
  if (!e) {
    return -1;
  }
  size_t len = e - p;
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, p, len);
  out[len] = '\0';
  return 0;
}

int json_parse(const char *line, app_cmd_t *out) {
  if (!line || !out) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->src_fd = -1; // why ????? should be argument

  if (count_substr(line, "\"cmd\"") != 1) {
    return -1;
  }

  char cmd[32] = {0};
  if (extract_string(line, "\"cmd\"", cmd, sizeof(cmd)) != 0) {
    return -1;
  }

  if (strcmp(cmd, "get_state") == 0) {
    out->type = APP_CMD_GET_STATE;
    return 0;
  }
  if (strcmp(cmd, "set_vol") == 0) {
    out->type = APP_CMD_SET_VOL;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "set_src") == 0) {
    char src_str[32] = {0};

    if (extract_string(line, "\"src\"", src_str, sizeof(src_str)) == 0) {
      if (strcmp(src_str, "net_radio") == 0) {
        out->type = APP_CMD_SET_SRC_NET_RADIO;
      } else if (strcmp(src_str, "net_server") == 0) {
        out->type = APP_CMD_SET_SRC_NET_SERVER;
      } else if (strcmp(src_str, "bt") == 0) {
        out->type = APP_CMD_SET_SRC_BT;
      } else {
        return -1;
      }
      return 0;
    }
  }
  if (strcmp(cmd, "set_net_rdo_url") == 0) {
    out->type = APP_CMD_CONFIG_NET_RDO_URL;
    extract_string(line, "\"url\"", out->data, sizeof(out->data));
    return 0;
  }
  if (strcmp(cmd, "config_net_srv_ip") == 0) {
    out->type = APP_CMD_CONFIG_NET_SRV_IP;
    extract_string(line, "\"ip\"", out->data, sizeof(out->data));
    return 0;
  }
  if (strcmp(cmd, "config_net_srv_port") == 0) {
    out->type = APP_CMD_CONFIG_NET_SRV_PORT;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "config_net_srv_format") == 0) {
    out->type = APP_CMD_CONFIG_NET_SRV_FORMAT;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "config_net_srv_rate") == 0) {
    out->type = APP_CMD_CONFIG_NET_SRV_RATE;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "config_bt_format") == 0) {
    out->type = APP_CMD_CONFIG_BT_RCV_FORMAT;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "config_bt_rate") == 0) {
    out->type = APP_CMD_CONFIG_BT_RCV_RATE;
    extract_int(line, "\"value\"", &out->value);
    return 0;
  }
  if (strcmp(cmd, "play") == 0) {
    out->type = APP_CMD_PLAY;
    return 0;
  }
  if (strcmp(cmd, "stop") == 0) {
    out->type = APP_CMD_STOP;
    return 0;
  }
  return -1;
}

int json_build_state(char *out, size_t out_size, const app_state_t *st) {
  if (!out || !st) {
    return -1;
  }
  return snprintf(
      out, out_size,
      "{\"type\":\"state\",\"vol\":%d,\"src\":%d,\"playing\":%s,\"url\":\"%"
      "s\",\"title\":\"%s\",\"artist\":\"%s\"}\n",
      st->volume, st->src, st->is_playing ? "true" : "false", st->net_rdo.url,
      st->title, st->artist);
}

int json_build_error(char *out, size_t out_size, const char *msg) {
  return snprintf(out, out_size, "{\"type\":\"error\",\"msg\":\"%s\"}\n",
                  msg ? msg : "unknown");
}

int json_build_broadcast(char *out, size_t out_size, const app_state_t *st,
                         app_cmd_type_t changed) {
  (void)changed;
  return json_build_state(out, out_size, st);
}
