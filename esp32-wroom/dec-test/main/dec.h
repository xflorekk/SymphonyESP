#ifndef DEC_H
#define DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  DEC_CODEC_MP3 = 0,
  DEC_CODEC_AAC,
  DEC_CODEC_FLAC,
  DEC_CODEC_WAV,
  DEC_CODEC_OPUS,
  DEC_CODEC_PCM,
  DEC_CODEC_AUTO,
} dec_codec_t;

typedef struct {
  dec_codec_t codec;
  uint32_t sample_rate; // tylko dla PCM
  uint8_t channels;     // tylko dla PCM
  uint8_t bits;         // tylko dla PCM
} dec_cfg_t;

typedef struct {
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits;
  uint32_t bitrate;
} dec_info_t;

typedef struct {
  uint32_t frame_cnt;
  uint32_t err_cnt;
  uint32_t bytes_in;
  uint32_t bytes_out;
  uint32_t sample_rate;
  uint8_t channels;
} dec_diag_t;

typedef struct dec_s {
  void *handle;
  dec_codec_t codec;
  dec_info_t info;
  volatile uint32_t frame_cnt;
  volatile uint32_t err_cnt;
  volatile uint32_t bytes_in;
  volatile uint32_t bytes_out;
} *dec_t;

dec_t dec_create(dec_cfg_t *cfg);
int dec_destroy(dec_t dec);
int dec_set_codec(dec_t dec, dec_codec_t codec);

int dec_decode(dec_t dec, const uint8_t *in, size_t in_len, int16_t *out,
               size_t out_cap, size_t *consumed, size_t *produced);

int dec_reset(dec_t dec);
int dec_flush(dec_t dec);
dec_info_t dec_get_info(dec_t dec);
dec_diag_t dec_get_diag(dec_t dec);
int dec_reset_diag(dec_t dec);

#endif