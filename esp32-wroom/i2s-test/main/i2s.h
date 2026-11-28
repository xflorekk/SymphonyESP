#ifndef I2S_H
#define I2S_H

#include <driver/i2s_common.h>
#include <driver/i2s_std.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int port;
  i2s_role_t role;
  bool is_tx;
  bool use_apll;
  int mclk_gpio;
  int bclk_gpio;
  int ws_gpio;
  int dout_gpio;
  int din_gpio;
  uint32_t rate;
  i2s_data_bit_width_t bits;
  i2s_slot_mode_t slot_mode;
  uint32_t dma_buf_count;
  uint32_t dma_buf_len;
} i2s_cfg_t;

typedef struct i2s_s {
  i2s_chan_handle_t handle;
  i2s_std_config_t std_cfg;
  i2s_chan_config_t chan_cfg;
  uint32_t rate;
  bool is_enabled;
  bool is_tx;
  bool use_apll;
  volatile uint32_t write_cnt;
  volatile uint32_t write_bytes;
  volatile uint32_t write_err_cnt;
  volatile uint32_t read_cnt;
  volatile uint32_t read_bytes;
  volatile uint32_t read_err_cnt;
  volatile uint32_t enable_cnt;
  volatile uint32_t reconfig_cnt;
} *i2s_t;

typedef struct {
  uint32_t write_cnt;
  uint32_t write_bytes;
  uint32_t write_err_cnt;
  uint32_t read_cnt;
  uint32_t read_bytes;
  uint32_t read_err_cnt;
  uint32_t enable_cnt;
  uint32_t reconfig_cnt;
  uint32_t rate;
  bool is_enabled;
} i2s_diag_t;

i2s_t i2s_create(i2s_cfg_t *cfg);
int i2s_reconfig_clk(i2s_t i2s, uint32_t rate);
int i2s_write(i2s_t i2s, const uint8_t *data, size_t len, size_t *written);
int i2s_read(i2s_t i2s, uint8_t *out, size_t len, size_t *read);
int i2s_enable(i2s_t i2s);
int i2s_disable(i2s_t i2s);
int i2s_destroy(i2s_t i2s);

i2s_diag_t i2s_get_diag(i2s_t i2s);
int i2s_reset_diag(i2s_t i2s);

#endif