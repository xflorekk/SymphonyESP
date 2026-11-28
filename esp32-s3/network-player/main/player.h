#ifndef PLAYER_H
#define PLAYER_H

#include "app_types.h"
#include "buf.h"
#include "dec.h"
#include "i2s.h"
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
  dec_codec_t codec;
  uint8_t channels;
  uint8_t bits;
  uint32_t rate;
} player_cmd_t;

typedef struct {
  size_t buf_in_size;  // 16384 dla MP3, 65536 dla FLAC
  size_t buf_out_size; // 32768 zawsze
  dec_cfg_t dec_cfg;   // codec na start
  i2s_cfg_t i2s_cfg;   // DAC
} player_cfg_t;

typedef struct {
  uint32_t buf_in_fill;
  uint32_t buf_out_fill;
  dec_diag_t dec;
  i2s_diag_t i2s;
} player_diag_t;

int player_init(const player_cfg_t *cfg);
int player_send_cmd(const player_cmd_t *cmd);
buf_t player_get_input_buf(void); // src tu pisze zakodowane
player_diag_t player_get_diag(void);

#endif