/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "i2s.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <soc/soc_caps.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "i2s";

/**
 *@brief Private definition of i2s handle
 *
 * Defined only here, not in i2s.h.
 */
struct i2s_s {
  i2s_chan_handle_t handle;
  i2s_std_config_t std_cfg;
  i2s_chan_config_t chan_cfg;
  i2s_data_bit_width_t bits;
  i2s_slot_mode_t slot_mode;
  uint32_t rate;
  bool is_enabled;
  bool is_tx;
  bool use_apll;
  i2s_diag_t diag;
};

static i2s_std_clk_config_t i2s_make_clk_config(uint32_t rate, bool use_apll) {
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
#if SOC_I2S_SUPPORTS_APLL
  ESP_LOGI(TAG, "APLL supported on this chip");
  if (use_apll) {
    clk.clk_src = I2S_CLK_SRC_APLL;
  }
#else
  // ESP32-S3 has no APLL, ignore use_apll
  if (use_apll) {
    ESP_LOGW(TAG, "APLL not supported on this chip, fallback to PLL_160M");
  }
  clk.clk_src = I2S_CLK_SRC_PLL_160M;
#endif
  clk.mclk_multiple = I2S_MCLK_MULTIPLE_384;
  return clk;
}

static i2s_err_t i2s_make_slot_config(uint8_t num_bits, uint8_t channels,
                                      i2s_data_bit_width_t *bits,
                                      i2s_slot_mode_t *slot_mode) {

  switch (num_bits) {
  case 16:
    *bits = I2S_DATA_BIT_WIDTH_16BIT;
    break;

  case 24:
    *bits = I2S_DATA_BIT_WIDTH_24BIT;
    break;

  default:
    return I2S_ERR_INVALID_ARG;
  }

  switch (channels) {
  case 1:
    *slot_mode = I2S_SLOT_MODE_MONO;
    break;

  case 2:
    *slot_mode = I2S_SLOT_MODE_STEREO;
    break;

  default:
    return I2S_ERR_INVALID_ARG;
  }
  return I2S_OK;
}

i2s_t i2s_create(const i2s_cfg_t *cfg) {
  if (!cfg) {
    return NULL;
  }
  i2s_t h = calloc(1, sizeof(*h));
  if (!h) {
    return NULL;
  }

  h->chan_cfg = (i2s_chan_config_t){
      .id = cfg->port,
      .role = cfg->role,
      .dma_desc_num = cfg->dma_buf_count,
      .dma_frame_num = cfg->dma_buf_len,
      .auto_clear = true,
  };
  if (i2s_new_channel(&h->chan_cfg, cfg->is_tx ? &h->handle : NULL,
                      cfg->is_tx ? NULL : &h->handle) != ESP_OK) {
    free(h);
    return NULL;
  }

  i2s_std_clk_config_t clk_cfg = i2s_make_clk_config(cfg->rate, cfg->use_apll);

  h->std_cfg = (i2s_std_config_t){
      .clk_cfg = clk_cfg,
      .slot_cfg =
          I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(cfg->bits, cfg->slot_mode),
      .gpio_cfg =
          {
              .mclk = cfg->mclk_gpio,
              .bclk = cfg->bclk_gpio,
              .ws = cfg->ws_gpio,
              .dout = cfg->is_tx ? cfg->dout_gpio : I2S_GPIO_UNUSED,
              .din = cfg->is_tx ? I2S_GPIO_UNUSED : cfg->din_gpio,
          },
  };
  h->use_apll = cfg->use_apll;
  h->bits = cfg->bits;
  h->slot_mode = cfg->slot_mode;

  if (i2s_channel_init_std_mode(h->handle, &h->std_cfg) != ESP_OK) {
    i2s_del_channel(h->handle);
    free(h);
    return NULL;
  }
  h->rate = cfg->rate;
  h->is_tx = cfg->is_tx;
  h->diag.rate = cfg->rate;
  h->diag.is_enabled = false;
  return h;
}

i2s_err_t i2s_reconfig(i2s_t i2s, uint32_t rate, uint8_t num_bits,
                       uint8_t channels) {
  if (!i2s || rate == 0) {
    return I2S_ERR_INVALID_ARG;
  }
  i2s_data_bit_width_t bits;
  i2s_slot_mode_t slot_mode;

  i2s_err_t ret = i2s_make_slot_config(num_bits, channels, &bits, &slot_mode);
  if (ret != I2S_OK) {
    return I2S_ERR_INVALID_ARG;
  }

  if (rate == i2s->rate && bits == i2s->bits && slot_mode == i2s->slot_mode) {
    return I2S_OK;
  }

  bool was_enabled = i2s->is_enabled;
  if (was_enabled) {
    i2s_channel_disable(i2s->handle);
  }

  i2s_std_clk_config_t clk = i2s_make_clk_config(rate, i2s->use_apll);
  esp_err_t err = i2s_channel_reconfig_std_clock(i2s->handle, &clk);
  if (err != ESP_OK) {
    if (was_enabled) {
      i2s_channel_enable(i2s->handle);
    }
    return I2S_ERR_FAIL;
  }

  i2s_std_slot_config_t slot =
      I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, slot_mode);
  err = i2s_channel_reconfig_std_slot(i2s->handle, &slot);
  if (err != ESP_OK) {
    if (was_enabled) {
      i2s_channel_enable(i2s->handle);
    }
    return I2S_ERR_FAIL;
  }

  i2s->std_cfg.clk_cfg = clk;
  i2s->std_cfg.slot_cfg = slot;
  i2s->rate = rate;
  i2s->bits = bits;
  i2s->slot_mode = slot_mode;
  i2s->diag.rate = rate;
  i2s->diag.reconfig_cnt++;
  if (was_enabled) {
    i2s_channel_enable(i2s->handle);
  }
  return I2S_OK;
}

i2s_err_t i2s_reconfig_clk(i2s_t i2s, uint32_t rate) {
  if (!i2s || rate == 0) {
    return I2S_ERR_INVALID_ARG;
  }
  bool was_enabled = i2s->is_enabled;
  if (was_enabled) {
    i2s_channel_disable(i2s->handle);
  }
  i2s_std_clk_config_t clk = i2s_make_clk_config(rate, i2s->use_apll);
  esp_err_t err = i2s_channel_reconfig_std_clock(i2s->handle, &clk);

  if (was_enabled) {
    i2s_channel_enable(i2s->handle);
  }
  if (err == ESP_OK) {
    i2s->rate = rate;
    i2s->diag.rate = rate;
    i2s->diag.reconfig_cnt++;
    return I2S_OK;
  }
  return I2S_ERR_FAIL;
}

i2s_err_t i2s_write(i2s_t i2s, const uint8_t *data, size_t len,
                    size_t *written) {
  if (!i2s || !i2s->is_tx || !data || !written) {
    return I2S_ERR_INVALID_ARG;
  }
  esp_err_t err =
      i2s_channel_write(i2s->handle, data, len, written, portMAX_DELAY);
  if (err == ESP_OK) {
    i2s->diag.write_cnt++;
    i2s->diag.write_bytes += *written;
    return I2S_OK;
  }
  i2s->diag.write_err_cnt++;
  return I2S_ERR_FAIL;
}

i2s_err_t i2s_read(i2s_t i2s, uint8_t *out, size_t len, size_t *read) {
  if (!i2s || i2s->is_tx || !out || !read) {
    return I2S_ERR_INVALID_ARG;
  }
  esp_err_t err = i2s_channel_read(i2s->handle, out, len, read, portMAX_DELAY);
  if (err == ESP_OK) {
    i2s->diag.read_cnt++;
    i2s->diag.read_bytes += *read;
    return I2S_OK;
  }
  i2s->diag.read_err_cnt++;
  return I2S_ERR_FAIL;
}

i2s_err_t i2s_enable(i2s_t i2s) {
  if (!i2s) {
    return I2S_ERR_INVALID_ARG;
  }
  if (i2s->is_enabled) {
    return I2S_OK;
  }
  esp_err_t err = i2s_channel_enable(i2s->handle);
  if (err != ESP_OK) {
    return I2S_ERR_FAIL;
  }
  i2s->is_enabled = true;
  i2s->diag.is_enabled = true;
  i2s->diag.enable_cnt++;
  return I2S_OK;
}

i2s_err_t i2s_disable(i2s_t i2s) {
  if (!i2s) {
    return I2S_ERR_INVALID_ARG;
  }
  if (!i2s->is_enabled) {
    return I2S_OK;
  }
  esp_err_t err = i2s_channel_disable(i2s->handle);
  if (err != ESP_OK) {
    return I2S_ERR_FAIL;
  }
  i2s->is_enabled = false;
  i2s->diag.is_enabled = false;
  return I2S_OK;
}

i2s_err_t i2s_destroy(i2s_t i2s) {
  if (!i2s) {
    return I2S_ERR_INVALID_ARG;
  }
  if (i2s->is_enabled) {
    i2s_channel_disable(i2s->handle);
  }
  i2s_del_channel(i2s->handle);
  free(i2s);
  return I2S_OK;
}

i2s_diag_t i2s_get_diag(i2s_t i2s) {
  i2s_diag_t d = {0};
  if (!i2s) {
    return d;
  }
  d = i2s->diag;
  d.rate = i2s->rate;
  d.is_enabled = i2s->is_enabled;
  return d;
}

i2s_err_t i2s_reset_diag(i2s_t i2s) {
  if (!i2s) {
    return I2S_ERR_INVALID_ARG;
  }
  uint32_t rate = i2s->diag.rate;
  bool enabled = i2s->diag.is_enabled;
  memset(&i2s->diag, 0, sizeof(i2s->diag));
  i2s->diag.rate = rate;
  i2s->diag.is_enabled = enabled;
  return I2S_OK;
}