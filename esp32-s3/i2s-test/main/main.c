#include "buf.h"
#include "i2s.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>

/*
    TEST 1: i2s.h, buf.h
    1. generate sinus samples
    2. write samples to buf
    3. read samples from buf
    4. write samples to i2s

    do it in loop with 5 second play, 1 second pause
    after each pause change rate between 44.1kHz and 48kHz


    TEST should include functions:
        i2s_create()
        i2s_reconfig_clk()
        i2s_write()
        i2s_enable()
        i2s_read()
        i2s_get_diag()
        i2s_reset_diag()
    */
static const char *TAG = "MAIN";

#define BUF_SIZE (16 * 1024)

static int16_t samples[512] = {0};
static float phase = 0;

static void gen_sine(uint32_t rate, float freq) {
  for (int i = 0; i < 256; i++) {
    int16_t s = (int16_t)(sinf(phase) * 10000);
    samples[i * 2] = s;
    samples[i * 2 + 1] = s;
    phase += 2 * 3.14159f * freq / rate;
    if (phase > 2 * 3.14159f) {
      phase -= 2 * 3.14159f;
    }
  }
}

void app_main(void) {
  i2s_cfg_t cfg;
  cfg = (i2s_cfg_t){
      .port = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .is_tx = true,
      .use_apll = true,
      .mclk_gpio = 4,
      .bclk_gpio = 5,
      .ws_gpio = 7,
      .dout_gpio = 6,
      .din_gpio = -1,
      .rate = 44100,
      .bits = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_mode = I2S_SLOT_MODE_STEREO,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
  };

  buf_t buf = buf_create(BUF_SIZE);
  i2s_t dac = i2s_create(&cfg);
  i2s_enable(dac);

  uint32_t freq = 440;
  uint32_t rate = 44100;
  while (1) {

    ESP_LOGI(TAG, "PLAY 5s rate=%lu frequency=%d", rate, freq);
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {

      gen_sine(rate, freq);
      buf_write(buf, (uint8_t *)samples, sizeof(samples));

      while (buf_get_fill(buf) > 0) {
        size_t sz;
        uint8_t *p = buf_read_up_to(buf, &sz, 1024);
        if (!p) {
          break;
        }
        size_t written = 0;
        i2s_write(dac, p, sz, &written);
        buf_return_item(buf, p);
      }
    }

    i2s_diag_t d = i2s_get_diag(dac);
    ESP_LOGI(
        TAG, "i2s diag: write_cnt=%lu bytes=%lu err=%lu reconfig=%lu rate=%lu",
        d.write_cnt, d.write_bytes, d.write_err_cnt, d.reconfig_cnt, d.rate);

    buf_diag_t b = buf_get_diag(buf);
    ESP_LOGI(TAG,
             " buf diag: max fill=%d occupancy=%d overrun=%d underrun=%d "
             "written=%d read=%d",
             b.max_fill, b.occupancy, b.overrun_cnt, b.underrun_cnt, b.written,
             b.read);

    i2s_reset_diag(dac);
    buf_reset_diag(buf);

    // flush buf??

    vTaskDelay(pdMS_TO_TICKS(1000));
    rate = (rate == 44100) ? 48000 : 44100;
    freq = (freq == 440) ? 880 : 440;
    ESP_LOGI(TAG, "reconfig to %lu", rate);

    i2s_reconfig_clk(dac, rate);
  }
}
