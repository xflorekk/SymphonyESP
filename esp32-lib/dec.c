/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dec.h"
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_audio_types.h>
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Private definition of decoder handle
 *
 * Defined only here, not in dec.h.
 */
struct dec_s {
  void *handle; // esp_audio_simple_dec_handle_t
  dec_codec_t codec;
  dec_info_t info;
  dec_diag_t diag;
};

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
    return ESP_AUDIO_SIMPLE_DEC_TYPE_RAW_OPUS;
  case DEC_CODEC_PCM:
    return -2;
  default:
    return -1;
  }
}

void dec_register_default(void) {
  esp_audio_dec_register_default();
  esp_audio_simple_dec_register_default();
}

dec_t dec_create(const dec_cfg_t *cfg) {
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
  if (esp_type == -2) {
    d->handle = NULL;
    d->codec = cfg->codec;
    d->info.sample_rate = cfg->sample_rate;
    d->info.channels = cfg->channels ? cfg->channels : 2;
    d->info.bits = cfg->bits ? cfg->bits : 16;
    return d;
  }
  esp_audio_simple_dec_cfg_t ecfg = {.dec_type = esp_type};
  esp_audio_err_t err = esp_audio_simple_dec_open(
      &ecfg, (esp_audio_simple_dec_handle_t *)&d->handle);
  if (err != ESP_AUDIO_ERR_OK) {
    free(d);
    return NULL;
  }
  d->codec = cfg->codec;
  return d;
}

dec_err_t dec_destroy(dec_t dec) {
  if (!dec) {
    return DEC_ERR_INVALID_ARG;
  }
  if (dec->handle) {
    esp_audio_simple_dec_close(dec->handle);
  }
  free(dec);
  return DEC_OK;
}

dec_err_t dec_set_codec(dec_t dec, dec_codec_t codec) {
  if (!dec) {
    return DEC_ERR_INVALID_ARG;
  }
  if (dec->codec == codec) {
    return DEC_OK;
  }
  int esp_type = dec_to_esp_type(codec);
  if (esp_type == -1) {
    return DEC_ERR_NOT_SUPPORTED;
  }
  if (dec->handle) {
    esp_audio_simple_dec_close(dec->handle);
  }
  dec->handle = NULL;
  dec->codec = codec;
  dec->info = (dec_info_t){0};
  if (esp_type == -2) {
    dec_reset_diag(dec);
    return DEC_OK;
  }
  esp_audio_simple_dec_cfg_t ecfg = {.dec_type = esp_type};
  esp_audio_err_t err = esp_audio_simple_dec_open(
      &ecfg, (esp_audio_simple_dec_handle_t *)&dec->handle);
  if (err != ESP_AUDIO_ERR_OK) {
    return DEC_ERR_FAIL;
  }
  dec_reset_diag(dec);
  return DEC_OK;
}

dec_err_t dec_decode(dec_t dec, const uint8_t *in, size_t in_len, uint8_t *out,
                     size_t out_cap, size_t *consumed, size_t *produced) {
  if (!dec || !in || !out || !consumed || !produced) {
    return DEC_ERR_INVALID_ARG;
  }
  *consumed = 0;
  *produced = 0;
  if (in_len == 0 || out_cap == 0) {
    return DEC_OK;
  }
  if (dec->codec == DEC_CODEC_PCM) {
    size_t copy = in_len < out_cap ? in_len : out_cap;
    memcpy(out, in, copy);
    *consumed = copy;
    *produced = copy;
    dec->diag.bytes_in += copy;
    dec->diag.bytes_out += copy;
    dec->diag.frame_cnt++;
    return DEC_OK;
  }
  if (!dec->handle) {
    return DEC_ERR_FAIL;
  }
  esp_audio_simple_dec_raw_t raw = {
      .buffer = (uint8_t *)in,
      .len = (uint32_t)in_len,
      .eos = false,
      .consumed = 0,
  };
  esp_audio_simple_dec_out_t frame = {
      .buffer = out,
      .len = (uint32_t)out_cap,
      .decoded_size = 0,
  };
  esp_audio_err_t err = esp_audio_simple_dec_process(dec->handle, &raw, &frame);
  if (err != ESP_AUDIO_ERR_OK) {
    dec->diag.err_cnt++;
    return DEC_ERR_DECODE;
  }
  *consumed = raw.consumed;
  *produced = frame.decoded_size;
  dec->diag.bytes_in += *consumed;
  dec->diag.bytes_out += *produced;
  if (*produced) {
    dec->diag.frame_cnt++;
  }
  if (*produced) {
    esp_audio_simple_dec_info_t info = {0};
    if (esp_audio_simple_dec_get_info(dec->handle, &info) == ESP_AUDIO_ERR_OK) {
      dec->info.sample_rate = info.sample_rate;
      dec->info.channels = info.channel;
      dec->info.bits = info.bits_per_sample;
      dec->info.bitrate = info.bitrate;
    }
  }
  return DEC_OK;
}

dec_err_t dec_reset(dec_t dec) {
  if (!dec) {
    return DEC_ERR_INVALID_ARG;
  }
  if (!dec->handle) {
    return DEC_OK; // PCM
  }
  if (esp_audio_simple_dec_reset(dec->handle) != ESP_AUDIO_ERR_OK) {
    return DEC_ERR_FAIL;
  }
  return DEC_OK;
}

dec_err_t dec_flush(dec_t dec) {
  if (!dec) {
    return DEC_ERR_INVALID_ARG;
  }
  return dec_reset(dec);
}

dec_info_t dec_get_info(dec_t dec) {
  dec_info_t i = {0};
  if (!dec) {
    return i;
  }
  return dec->info;
}

dec_diag_t dec_get_diag(dec_t dec) {
  dec_diag_t d = {0};
  if (!dec) {
    return d;
  }
  d = dec->diag;
  d.sample_rate = dec->info.sample_rate;
  d.channels = dec->info.channels;
  return d;
}

dec_err_t dec_reset_diag(dec_t dec) {
  if (!dec) {
    return DEC_ERR_INVALID_ARG;
  }
  dec->diag.frame_cnt = 0;
  dec->diag.err_cnt = 0;
  dec->diag.bytes_in = 0;
  dec->diag.bytes_out = 0;
  return DEC_OK;
}