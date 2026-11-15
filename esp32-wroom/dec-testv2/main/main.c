#include "buf.h"
#include "dec.h"
#include "i2s.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define BUF_IN_SIZE (32 * 1024)
#define BUF_OUT_SIZE (32 * 1024)

static const char *TAG = "main";

extern const uint8_t test_5s_mp3_start[] asm("_binary_test_5s_mp3_start");
extern const uint8_t test_5s_mp3_end[] asm("_binary_test_5s_mp3_end");
extern const uint8_t test_5s_aac_start[] asm("_binary_test_5s_aac_start");
extern const uint8_t test_5s_aac_end[] asm("_binary_test_5s_aac_end");
extern const uint8_t test_5s_flac_start[] asm("_binary_test_5s_flac_start");
extern const uint8_t test_5s_flac_end[] asm("_binary_test_5s_flac_end");

static uint8_t tmp_in[16384];
static uint8_t tmp_pcm[32768];
static uint8_t pending[16384];

static void drain_to_dac(i2s_t dac, buf_t buf_out) {
  while (buf_get_fill(buf_out) > 0) {
    size_t item_size = 0;
    void *item = buf_read_up_to(buf_out, &item_size, 4096);
    if (!item || item_size == 0) {
      break;
    }
    size_t off = 0;
    while (off < item_size) {
      size_t written = 0;
      if (i2s_write(dac, (uint8_t *)item + off, item_size - off, &written) !=
          I2S_OK) {
        break;
      }
      if (written == 0) {
        break;
      }
      off += written;
    }
    buf_return_item(buf_out, item);
  }
}

// Jedna sesja odtwarzania jednego pliku, jednym kodekiem.
// Player kontroluje pending, dec tylko dekoduje.
static void play_one(dec_t dec, buf_t buf_in, buf_t buf_out, i2s_t dac,
                     const uint8_t *data, size_t len, const char *name) {
  ESP_LOGI(TAG, "PLAY %s %u bytes", name, (unsigned)len);

  dec_reset(dec);
  buf_flush(buf_in);
  buf_flush(buf_out);

  size_t pos = 0;
  size_t pending_len = 0;
  size_t total_produced = 0;
  int no_progress = 0;

  uint32_t last_rate = 0;
  uint8_t last_bits = 0;
  uint8_t last_channels = 0;

  while (1) {
    // 1. producent -> buf_in, po 1024
    if (pos < len) {
      size_t free_in = buf_get_free(buf_in);
      if (free_in >= 1024) {
        size_t want = len - pos;
        if (want > 1024) {
          want = 1024;
        }
        if (buf_write(buf_in, data + pos, want) == BUF_OK) {
          pos += want;
        }
      }
    }

    // 2. player: pending + buf_in -> tmp_in -> dec_decode -> pending
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

        dec_err_t rc = dec_decode(dec, tmp_in, total_in, tmp_pcm,
                                  sizeof(tmp_pcm), &consumed, &produced);
        if (rc != DEC_OK) {
          ESP_LOGE(TAG, "dec_decode err %s", name);
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
          dec_info_t info = dec_get_info(dec);
          if (info.sample_rate != 0 && info.bits != 0 && info.channels != 0) {
            if (info.sample_rate != last_rate || info.bits != last_bits ||
                info.channels != last_channels) {
              // Twoja nowa wersja: i2s_reconfig bierze liczby, nie enumy
              // walidacja jest w środku I2S, player nie includuje drivera
              if (i2s_reconfig(dac, info.sample_rate, info.bits,
                               info.channels) == I2S_OK) {
                last_rate = info.sample_rate;
                last_bits = info.bits;
                last_channels = info.channels;
                ESP_LOGI(TAG, "i2s reconfig: %lu Hz %u bit %u ch",
                         (unsigned long)last_rate, last_bits, last_channels);
              }
            }
          }

          // TEST GŁOŚNOŚCI -6dB
          /*
          int16_t *pcm = (int16_t *)tmp_pcm;
          size_t samples = produced / 2;
          for (size_t i = 0; i < samples; i++) {
            int32_t s = pcm[i];
            s = s / 2;
            pcm[i] = (int16_t)s;
          }
          */

          if (last_bits == 24) {
            size_t num_samples = produced / 3;
            // od końca, bo rozszerzamy 3 -> 4 w tym samym buforze
            // od początku byśmy nadpisali dane, których jeszcze nie
            // przeczytaliśmy
            for (int i = (int)num_samples - 1; i >= 0; i--) {
              uint8_t *src = tmp_pcm + i * 3;
              int32_t v = src[0] | (src[1] << 8) | (src[2] << 16);
              // sign-extend 24 -> 32
              if (v & 0x800000) {
                v |= 0xFF000000;
              }
              v = v / 2;  // -6dB, tak samo jak dla 16-bit
              v = v << 8; // left-justify: I2S 24-bit czyta górne 24 bity z
                          // slotu 32-bit
              int32_t *dst = (int32_t *)(tmp_pcm + i * 4);
              *dst = v;
            }
            produced = num_samples *
                       4; // <-- ZMIANA: produced jest teraz w formacie I2S
          } else {
            // 16-bit - stary TEST GŁOŚNOŚCI, teraz aktywny, nie zakomentowany
            int16_t *pcm = (int16_t *)tmp_pcm;
            size_t samples = produced / 2;
            for (size_t i = 0; i < samples; i++) {
              int32_t s = pcm[i];
              s = s / 2; // -6dB
              pcm[i] = (int16_t)s;
            }
          }

          no_progress = 0;
          total_produced += produced;
          size_t off = 0;
          while (off < produced) {
            size_t w =
                buf_write_partial(buf_out, tmp_pcm + off, produced - off);
            if (w == 0) {
              drain_to_dac(dac, buf_out);
            } else {
              off += w;
            }
          }
        } else {
          if (consumed == 0) {
            no_progress++;
          } else {
            no_progress = 0;
          }
        }
      }
    }

    // 3. drain buf_out -> i2s
    drain_to_dac(dac, buf_out);

    if (pos >= len && buf_get_fill(buf_in) == 0 && pending_len == 0 &&
        buf_get_fill(buf_out) == 0) {
      break;
    }
    if (no_progress > 100) {
      ESP_LOGW(TAG, "no progress, abort %s", name);
      break;
    }
  }

  drain_to_dac(dac, buf_out);

  // --- DIAGNOSTYKA ---
  i2s_diag_t d = i2s_get_diag(dac);
  ESP_LOGI(TAG,
           "i2s diag: write_cnt=%lu bytes=%lu err=%lu reconfig=%lu rate=%lu",
           (unsigned long)d.write_cnt, (unsigned long)d.write_bytes,
           (unsigned long)d.write_err_cnt, (unsigned long)d.reconfig_cnt,
           (unsigned long)d.rate);

  buf_diag_t bi = buf_get_diag(buf_in);
  ESP_LOGI(TAG,
           " buf_in diag: max fill=%u occupancy=%u overrun=%lu "
           "underrun=%lu written=%lu read=%lu",
           (unsigned)bi.max_fill, (unsigned)bi.occupancy,
           (unsigned long)bi.overrun_cnt, (unsigned long)bi.underrun_cnt,
           (unsigned long)bi.written, (unsigned long)bi.read);

  buf_diag_t bo = buf_get_diag(buf_out);
  ESP_LOGI(TAG,
           " buf_out diag: max fill=%u occupancy=%u overrun=%lu "
           "underrun=%lu written=%lu read=%lu",
           (unsigned)bo.max_fill, (unsigned)bo.occupancy,
           (unsigned long)bo.overrun_cnt, (unsigned long)bo.underrun_cnt,
           (unsigned long)bo.written, (unsigned long)bo.read);

  dec_diag_t dd = dec_get_diag(dec);
  ESP_LOGI(TAG,
           " dec diag: %s frames=%lu err=%lu bytes_in=%lu bytes_out=%lu "
           "produced=%u sr=%lu ch=%u",
           name, (unsigned long)dd.frame_cnt, (unsigned long)dd.err_cnt,
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

void app_main(void) {
  buf_t buf_in = buf_create(BUF_IN_SIZE);
  buf_t buf_out = buf_create(BUF_OUT_SIZE);

  i2s_cfg_t i2s_cfg;
  i2s_cfg = (i2s_cfg_t){
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

  dec_register_default();
  dec_t dec = dec_create(&(dec_cfg_t){.codec = DEC_CODEC_MP3});
  if (!dec) {
    ESP_LOGE(TAG, "dec_create() FAIL!");
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }

  size_t mp3_len = test_5s_mp3_end - test_5s_mp3_start;
  size_t aac_len = test_5s_aac_end - test_5s_aac_start;
  size_t flac_len = test_5s_flac_end - test_5s_flac_start;
  ESP_LOGI(TAG, "embedded mp3: %u bytes, aac: %u bytes, flac: %u bytes",
           (unsigned)mp3_len, (unsigned)aac_len, (unsigned)flac_len);

  while (1) {
    // MP3
    play_one(dec, buf_in, buf_out, dac, test_5s_mp3_start, mp3_len, "mp3");

    // Przełączenie kodeka MP3 -> AAC, player czyści pending w play_one
    if (dec_set_codec(dec, DEC_CODEC_AAC) != DEC_OK) {
      ESP_LOGE(TAG, "dec_set_codec AAC FAIL");
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // AAC
    play_one(dec, buf_in, buf_out, dac, test_5s_aac_start, aac_len, "aac");

    // Powrót AAC -> FLAC
    if (dec_set_codec(dec, DEC_CODEC_FLAC) != DEC_OK) {
      ESP_LOGE(TAG, "dec_set_codec FLAC FAIL");
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // FLAC
    play_one(dec, buf_in, buf_out, dac, test_5s_flac_start, flac_len, "flac");

    // Powrót FLAC -> MP3
    if (dec_set_codec(dec, DEC_CODEC_MP3) != DEC_OK) {
      ESP_LOGE(TAG, "dec_set_codec MP3 FAIL");
      for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }
  }
}