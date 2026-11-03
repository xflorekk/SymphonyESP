#ifndef APP_TYPES_H
#define APP_TYPES_H

typedef enum {
  APP_CMD_GET_STATE = 0,
  //
  APP_CMD_SET_SRC_NET_RADIO,
  APP_CMD_SET_SRC_NET_SERVER,
  APP_CMD_SET_SRC_BT,
  APP_CMD_SET_URL,
  APP_CMD_SET_SAMPLE,
  APP_CMD_SET_FORMAT,
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
} app_cmd_type_t;

#endif