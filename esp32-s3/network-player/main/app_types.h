#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <stdint.h>

typedef enum {
  APP_SRC_NONE = 0,
  APP_SRC_NET_RDO,
  APP_SRC_NET_SRV,
  APP_SRC_BT_RCV,
} app_src_t;

typedef enum {
  APP_CMD_GET_STATE = 0,
  //
  APP_CMD_SET_SRC_NET_RADIO,
  APP_CMD_SET_SRC_NET_SERVER,
  APP_CMD_SET_SRC_BT,
  //
  APP_CMD_CONFIG_NET_RDO_URL,
  APP_CMD_CONFIG_NET_SRV_IP,
  APP_CMD_CONFIG_NET_SRV_PORT,
  APP_CMD_CONFIG_NET_SRV_FORMAT,
  APP_CMD_CONFIG_NET_SRV_RATE,
  APP_CMD_CONFIG_BT_RCV_FORMAT,
  APP_CMD_CONFIG_BT_RCV_RATE,
  //
  APP_CMD_TOGGLE_PLAY,
  APP_CMD_PLAY,
  APP_CMD_STOP,
  APP_CMD_PREV,
  APP_CMD_NEXT,
  //  Audio parameters
  APP_CMD_SET_VOL,  // volume
  APP_CMD_SET_BASS, // bass
  APP_CMD_SET_TRB,  // treble
  APP_CMD_SET_BLN,  // balance
  APP_CMD_SET_MUTE, // mute
  APP_CMD_SET_DIR,  // pure direct
  //  Events
  APP_EVT_BT_CONNECTED,
  APP_EVT_BT_DISCONNECTED,
  //
  APP_CMD_META_ARTIST,
  APP_CMD_META_TITLE,
  //
  APP_CMD_NET_RDO_FAIL,
  APP_CMD_NET_SRV_FAIL,
  APP_CMD_BT_RCV_FAIL,
} app_cmd_type_t;

#define CFG_NET_RDO_URL (1 << 0)
#define CFG_NET_SRV_IP (1 << 1)
#define CFG_NET_SRV_PORT (1 << 2)
#define CFG_NET_SRV_FORMAT (1 << 3)
#define CFG_NET_SRV_RATE (1 << 4)
#define CFG_BT_RCV_FORMAT (1 << 5)
#define CFG_BT_RCV_RATE (1 << 6)

#define CFG_NET_RDO_REQUIRED (CFG_NET_RDO_URL)
#define CFG_NET_SRV_REQUIRED                                                   \
  (CFG_NET_SRV_IP | CFG_NET_SRV_PORT | CFG_NET_SRV_FORMAT | CFG_NET_SRV_RATE)
#define CFG_BT_RCV_REQUIRED (CFG_BT_RCV_FORMAT | CFG_BT_RCV_RATE)

typedef enum {
  PLAYER_FORMAT_PCM = 0,
  PLAYER_FORMAT_MP3,
} player_format_t;

typedef struct {
  char srv_ip[32];
  uint16_t srv_port;
  player_format_t format;
  uint32_t rate;
} net_srv_cfg_t;

typedef struct {
  char url[256];
  player_format_t format;
  uint32_t rate;
} net_rdo_cfg_t;

typedef struct {
  player_format_t format;
  uint32_t rate;
} bt_rcv_cfg_t;

typedef struct {
  app_src_t src;
  bool src_ready;

  // SRC CONFIG
  net_rdo_cfg_t net_rdo;
  net_srv_cfg_t net_srv;
  bt_rcv_cfg_t bt_rcv;

  //  AUDIO
  int volume;
  int bass;
  int treble;
  int balance;
  bool is_muted;
  bool is_playing;
  //  METADATA
  char src_name[64];
  char title[128];
  char artist[128];
} app_state_t;

typedef struct {
  app_cmd_type_t type;
  int src_fd;
  int value;
  char data[256];
} app_cmd_t;

#endif
