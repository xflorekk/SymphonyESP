#include "dec.h"
#include "esp_audio_simple_dec.h"
#include <stdlib.h>
#include <string.h>

static int dec_to_esp_type(dec_codec_t c) {
  switch (c) {
  case DEC_CODEC_MP3:
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  case DEC_CODEC_AAC:
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  case DEC_CODEC_FLAC:
    return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
  case DEC_CODEC_WAV:
    return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  case DEC_CODEC_OPUS:
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OPUS;
  case DEC_CODEC_PCM:
    return -2;
  default:
    return -1;
  }
}

dec_t dec_create(dec_cfg_t *cfg) {
  if (!cfg) {
    return NULL;
  }
  int esp_type = dec_to_esp_type(cfg->codec);
  if (esp_type == -1) {
    return NULL;
  }

  dec_t d = calloc(1, sizeof(*d));
  if (!d) {
    return NULL;
  }

  // PCM - without handle
  if (esp_type == -2) {
    d->handle = NULL;
    d->info.sample_rate = cfg->sample_rate;
    d->info.channels = cfg->channels ? cfg->channels : 2;
    d->info.bits = cfg->bits ? cfg->bits : 16;
    return d;
  }

  esp_audio_simple_dec_cfg_t ecfg = {.dec_type = esp_type};
  esp_err_t err = esp_audio_simple_dec_open(
      &ecfg, (esp_audio_simple_dec_handle_t *)&d->handle);
  if (err != ESP_OK) {
    free(d);
    return NULL;
  }
  d->codec = cfg->codec;
  return d;
}

int dec_destroy(dec_t dec) {
  if (!dec) {
    return -1;
  }
  if (dec->handle) {
    esp_audio_simple_dec_close(dec->handle);
  }
  free(dec);
  return 0;
}

int dec_set_codec(dec_t dec, dec_codec_t codec) {
  if (!dec) {
    return -1;
  }
  if (dec->codec == codec) {
    return 0;
  }
  int esp_type = dec_to_esp_type(codec);
  if (esp_type == -1) {
    return -1;
  }
  if (dec->handle) {
    esp_audio_simple_dec_close(dec->handle);
  }
  dec->handle = NULL;
  dec->codec = codec;

  if (esp_type == -2) {
    dec_reset_diag(dec);
    return 0;
  }

  esp_audio_simple_dec_cfg_t ecfg = {.dec_type = esp_type};
  esp_err_t err = esp_audio_simple_dec_open(
      &ecfg, (esp_audio_simple_dec_handle_t *)&dec->handle);
  if (err != ESP_OK) {
    return -1;
  }
  dec->codec = codec;
  dec_reset_diag(dec);
  return 0;
}

int dec_decode(dec_t dec, const uint8_t *in, size_t in_len, int16_t *out,
               size_t out_cap, size_t *consumed, size_t *produced) {

  if (!dec || !in || !out || !consumed || !produced) {
    return -1;
  }
  *consumed = 0;
  *produced = 0;
  if (in_len == 0 || out_cap == 0) {
    return 0;
  }

  // PCM - passthrough
  if (dec->codec == DEC_CODEC_PCM) {
    size_t copy = in_len < out_cap ? in_len : out_cap;
    memcpy(out, in, copy);
    *consumed = copy;
    *produced = copy;
    dec->bytes_in += copy;
    dec->bytes_out += copy;
    dec->frame_cnt++;
    return 0;
  }

  if (!dec->handle) {
    return -1;
  }
  esp_audio_simple_dec_in_t in_data = {
      .data = (uint8_t *)in, .len = in_len, .consumed = 0};
  esp_audio_simple_dec_out_t out_data = {.data = (uint8_t *)out,
                                         .len = out_cap};

  esp_err_t err = esp_audio_simple_dec_decode(dec->handle, &in_data, &out_data);
  if (err != ESP_OK) {
    dec->err_cnt++;
    return -1;
  }

  *consumed = in_data.consumed;
  *produced = out_data.len;
  dec->bytes_in += *consumed;
  dec->bytes_out += *produced;
  dec->frame_cnt++;
  dec->info.sample_rate = out_data.sample_rate;
  dec->info.channels = out_data.channel;
  dec->info.bits = out_data.bits_per_sample;
  dec->info.bitrate = 0;

  return 0;
}

int dec_reset(dec_t dec) {
  if (!dec) {
    return -1;
  }
  // PCM - nothing to reset
  if (!dec->handle) {
    return 0;
  }

  dec_codec_t c = dec->codec;
  if (dec->handle) {
    esp_audio_simple_dec_close(dec->handle);
  }
  dec->handle = NULL;
  esp_audio_simple_dec_cfg_t ecfg = {.dec_type = dec_to_esp_type(c)};
  esp_err_t err = esp_audio_simple_dec_open(
      &ecfg, (esp_audio_simple_dec_handle_t *)&dec->handle);
  if (err != ESP_OK) {
    return -1;
  }
  return 0;
}

int dec_flush(dec_t dec) {
  if (!dec) {
    return -1;
  }
  return dec_reset(dec);
}

dec_info_t dec_get_info(dec_t dec) {
  dec_info_t i = {0};
  if (!dec) {
    return i;
  }
  i = dec->info;
  return i;
}

dec_diag_t dec_get_diag(dec_t dec) {
  dec_diag_t d = {0};
  if (!dec) {
    return d;
  }
  d.frame_cnt = dec->frame_cnt;
  d.err_cnt = dec->err_cnt;
  d.bytes_in = dec->bytes_in;
  d.bytes_out = dec->bytes_out;
  d.sample_rate = dec->info.sample_rate;
  d.channels = dec->info.channels;

  return d;
}

int dec_reset_diag(dec_t dec) {
  if (!dec) {
    return -1;
  }
  dec->frame_cnt = 0;
  dec->err_cnt = 0;
  dec->bytes_in = 0;
  dec->bytes_out = 0;
  return 0;
}