#include "buf.h"
#include "dec.h"
#include "i2s.h"
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define BUF_IN_SIZE (32 * 1024)
#define BUF_OUT_SIZE (32 * 1024)

/*
    DECODER TEST:
    using modules: buf, i2s, dec
    target: decode and play mp3 fragment
    1. buf_in, buf_out, dac, dec init
    while(1)
    2. write data to buf_in
    3. read data from buf_in
    4. decode data from mp3 to pcm
    5. write pcm to buf_out
    6. read buf_out
    7. write i2s to dac

    play 5s then wait for 1s, play again
*/

static const char *TAG = "MAIN";

extern const uint8_t test_5s_mp3_start[] asm("_binary_test_5s_mp3_start");
extern const uint8_t test_5s_mp3_end[] asm("_binary_test_5s_mp3_end");

static uint8_t tmp_in[4096];
static uint8_t tmp_pcm[8192];
static uint8_t pending[4096];

static void drain_to_dac(i2s_t dac, buf_t buf_out) {
  while (buf_get_fill(buf_out) > 0) {
    size_t item_size = 0;
    void *item = buf_read_up_to(buf_out, &item_size, 4096);
    if (!item || item_size == 0)
      break;
    size_t off = 0;
    while (off < item_size) {
      size_t written = 0;
      if (i2s_write(dac, (uint8_t *)item + off, item_size - off, &written) != 0)
        break;
      if (written == 0)
        break;
      off += written;
    }
    buf_return_item(buf_out, item);
  }
}

void app_main(void) {
  i2s_cfg_t i2s_cfg = (i2s_cfg_t){
      .port = I2S_NUM_0,
      .role = I2S_ROLE_MASTER,
      .is_tx = true,
      .use_apll = true,
      .mclk_gpio = 0,
      .bclk_gpio = 21,
      .ws_gpio = 18,
      .dout_gpio = 19,
      .din_gpio = -1,
      .rate = 44100,
      .bits = I2S_DATA_BIT_WIDTH_16BIT,
      .slot_mode = I2S_SLOT_MODE_STEREO,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
  };
  i2s_t dac = i2s_create(&i2s_cfg);
  i2s_enable(dac);

  buf_t buf_in = buf_create(BUF_IN_SIZE);
  buf_t buf_out = buf_create(BUF_OUT_SIZE);

  esp_audio_dec_register_default();
  esp_audio_simple_dec_register_default();
  dec_t dec = dec_create(&(dec_cfg_t){.codec = DEC_CODEC_MP3});
  if (!dec) {
    ESP_LOGE(TAG, "dec_create failed!");
    for (;;)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  const uint8_t *mp3 = test_5s_mp3_start;
  size_t mp3_len = test_5s_mp3_end - test_5s_mp3_start;
  ESP_LOGI(TAG, "embedded mp3: %u bytes", (unsigned)mp3_len);

  while (1) {
    ESP_LOGI(TAG, "PLAY mp3 5s");

    dec_reset(dec);
    buf_flush(buf_in);
    buf_flush(buf_out);

    size_t mp3_pos = 0;
    size_t pending_len = 0;
    size_t total_produced = 0;
    int no_progress = 0;

    while (1) {
      // 2. write -> buf_in
      if (mp3_pos < mp3_len) {
        size_t free_in = BUF_IN_SIZE - buf_get_fill(buf_in);
        if (free_in >= 1024) {
          size_t want = mp3_len - mp3_pos;
          if (want > 1024)
            want = 1024;
          if (buf_write(buf_in, mp3 + mp3_pos, want))
            mp3_pos += want;
        }
      }

      // 3+4. read buf_in -> decode -> write buf_out
      size_t fill_in = buf_get_fill(buf_in);
      if (fill_in > 0 || pending_len > 0) {
        size_t total_in = pending_len;
        memcpy(tmp_in, pending, pending_len);
        if (fill_in > 0) {
          size_t item_size = 0;
          void *item =
              buf_read_up_to(buf_in, &item_size, sizeof(tmp_in) - pending_len);
          if (item) {
            memcpy(tmp_in + pending_len, item, item_size);
            total_in += item_size;
            buf_return_item(buf_in, item);
          }
        }
        if (total_in > 0) {
          size_t consumed = 0, produced = 0;
          int rc = dec_decode(dec, tmp_in, total_in, (int16_t *)tmp_pcm,
                              sizeof(tmp_pcm), &consumed, &produced);
          if (rc != 0) {
            ESP_LOGE(TAG, "dec_decode err");
            break;
          }
          if (consumed < total_in) {
            size_t left = total_in - consumed;
            memmove(pending, tmp_in + consumed, left);
            pending_len = left;
          } else {
            pending_len = 0;
          }
          if (produced > 0) {
            no_progress = 0;
            size_t off = 0;
            while (off < produced) {
              size_t chunk = produced - off;
              if (chunk > 1024)
                chunk = 1024;
              if (!buf_write(buf_out, tmp_pcm + off, chunk)) {
                drain_to_dac(dac, buf_out);
              } else {
                off += chunk;
              }
            }
            total_produced += produced;
          } else {
            if (consumed == 0)
              no_progress++;
            else
              no_progress = 0;
          }
        }
      }

      // 6+7. read buf_out -> i2s
      drain_to_dac(dac, buf_out);

      if (mp3_pos >= mp3_len && buf_get_fill(buf_in) == 0 && pending_len == 0 &&
          buf_get_fill(buf_out) == 0)
        break;
      if (no_progress > 100) {
        ESP_LOGW(TAG, "no progress, abort");
        break;
      }
    }

    drain_to_dac(dac, buf_out);

    // --- DIAGNOSTYKA jak w teście sinusa ---
    i2s_diag_t d = i2s_get_diag(dac);
    ESP_LOGI(TAG,
             "i2s diag: write_cnt=%lu bytes=%lu err=%lu reconfig=%lu rate=%lu",
             (unsigned long)d.write_cnt, (unsigned long)d.write_bytes,
             (unsigned long)d.write_err_cnt, (unsigned long)d.reconfig_cnt,
             (unsigned long)d.rate);

    buf_diag_t bi = buf_get_diag(buf_in);
    ESP_LOGI(TAG,
             " buf_in diag: max fill=%u occupancy=%u overrun=%lu underrun=%lu "
             "written=%lu read=%lu",
             (unsigned)bi.max_fill, (unsigned)bi.occupancy,
             (unsigned long)bi.overrun_cnt, (unsigned long)bi.underrun_cnt,
             (unsigned long)bi.written, (unsigned long)bi.read);

    buf_diag_t bo = buf_get_diag(buf_out);
    ESP_LOGI(TAG,
             " buf_out diag: max fill=%u occupancy=%u overrun=%lu underrun=%lu "
             "written=%lu read=%lu",
             (unsigned)bo.max_fill, (unsigned)bo.occupancy,
             (unsigned long)bo.overrun_cnt, (unsigned long)bo.underrun_cnt,
             (unsigned long)bo.written, (unsigned long)bo.read);

    dec_diag_t dd = dec_get_diag(dec);
    ESP_LOGI(TAG,
             " dec diag: frames=%lu err=%lu bytes_in=%lu bytes_out=%lu "
             "produced=%u sr=%lu ch=%u",
             (unsigned long)dd.frame_cnt, (unsigned long)dd.err_cnt,
             (unsigned long)dd.bytes_in, (unsigned long)dd.bytes_out,
             (unsigned)total_produced, (unsigned long)dd.sample_rate,
             dd.channels);

    i2s_reset_diag(dac);
    buf_reset_diag(buf_in);
    buf_reset_diag(buf_out);
    dec_reset_diag(dec);

    ESP_LOGI(TAG, "pause 1s");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}