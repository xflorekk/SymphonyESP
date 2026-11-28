#include "i2s.h"
#include <freertos/FreeRTOS.h>
#include <stdlib.h>
#include <string.h>

i2s_t i2s_create(i2s_cfg_t *cfg) {
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

  h->std_cfg = (i2s_std_config_t){
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->rate),
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
  if (cfg->use_apll) {
    h->std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  }
  h->use_apll = cfg->use_apll;

  if (i2s_channel_init_std_mode(h->handle, &h->std_cfg) != ESP_OK) {
    i2s_del_channel(h->handle);
    free(h);
    return NULL;
  }
  h->rate = cfg->rate;
  h->is_tx = cfg->is_tx;
  return h;
}

int i2s_reconfig_clk(i2s_t i2s, uint32_t rate) {
  if (!i2s) {
    return -1;
  }
  bool was_enabled = i2s->is_enabled;
  if (was_enabled) {
    i2s_channel_disable(i2s->handle);
  }
  i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  if (i2s->use_apll) {
    clk.clk_src = I2S_CLK_SRC_APLL;
  }
  esp_err_t err = i2s_channel_reconfig_std_clock(i2s->handle, &clk);

  if (was_enabled) {
    i2s_channel_enable(i2s->handle);
  }
  if (err == ESP_OK) {
    i2s->rate = rate;
    i2s->reconfig_cnt++;
    return 0;
  }
  return -1;
}

int i2s_write(i2s_t i2s, const uint8_t *data, size_t len, size_t *written) {
  if (!i2s || !i2s->is_tx) {
    return -1;
  }
  esp_err_t err =
      i2s_channel_write(i2s->handle, data, len, written, portMAX_DELAY);
  if (err == ESP_OK) {
    i2s->write_cnt++;
    i2s->write_bytes += *written;
    return 0;
  }
  i2s->write_err_cnt++;
  return -1;
}

int i2s_read(i2s_t i2s, uint8_t *out, size_t len, size_t *read) {
  if (!i2s || i2s->is_tx) {
    return -1;
  }
  esp_err_t err = i2s_channel_read(i2s->handle, out, len, read, portMAX_DELAY);
  if (err == ESP_OK) {
    i2s->read_cnt++;
    i2s->read_bytes += *read;
    return 0;
  }
  i2s->read_err_cnt++;
  return -1;
}

int i2s_enable(i2s_t i2s) {
  if (!i2s || i2s->is_enabled) {
    return 0;
  }
  esp_err_t err = i2s_channel_enable(i2s->handle);
  if (err != ESP_OK) {
    return -1;
  }
  i2s->is_enabled = true;
  i2s->enable_cnt++;
  return 0;
}

int i2s_disable(i2s_t i2s) {
  if (!i2s || !i2s->is_enabled) {
    return 0;
  }
  esp_err_t err = i2s_channel_disable(i2s->handle);
  if (err != ESP_OK) {
    return -1;
  }
  i2s->is_enabled = false;
  return 0;
}

int i2s_destroy(i2s_t i2s) {
  if (!i2s) {
    return -1;
  }
  i2s_channel_disable(i2s->handle);
  i2s_del_channel(i2s->handle);
  free(i2s);
  return 0;
}

i2s_diag_t i2s_get_diag(i2s_t i2s) {
  i2s_diag_t d = {0};
  if (!i2s) {
    return d;
  }
  d.write_cnt = i2s->write_cnt;
  d.write_bytes = i2s->write_bytes;
  d.write_err_cnt = i2s->write_err_cnt;
  d.read_cnt = i2s->read_cnt;
  d.read_bytes = i2s->read_bytes;
  d.read_err_cnt = i2s->read_err_cnt;
  d.enable_cnt = i2s->enable_cnt;
  d.reconfig_cnt = i2s->reconfig_cnt;
  d.rate = i2s->rate;
  d.is_enabled = i2s->is_enabled;
  return d;
}

int i2s_reset_diag(i2s_t i2s) {
  if (!i2s) {
    return -1;
  }
  i2s->write_cnt = i2s->write_bytes = i2s->write_err_cnt = 0;
  i2s->read_cnt = i2s->read_bytes = i2s->read_err_cnt = 0;
  i2s->enable_cnt = i2s->reconfig_cnt = 0;
  return 0;
}