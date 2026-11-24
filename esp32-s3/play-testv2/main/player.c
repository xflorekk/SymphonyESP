/**
 * @file player.c
 * @brief SymphonyESP Player implementation - task + state machine +
 * command/ACK.
 *
 * @details Pattern (same as lab, proven in lab test):
 * - player_init() creates static queue + event group, no heap.
 * - player_start() creates static pinned task, no heap.
 * - player_task() loop: 1) receive command (blocking in
 * IDLE/UNCONFIGURED/PAUSED, non-blocking in PLAYING/CONFIGURING), 2) validate
 * command vs state, 3) set pending, 4) dispatch to state_*(), 5) on error ->
 * state_error().
 * - pending is single enum, not should_* bools. Only one pending request at a
 * time.
 * - ACK is shared ACK_OK / ACK_FAIL / ACK_BUSY, not per-command bits.
 *   API is blocking, caller knows which command it sent, so bit is only a
 * signal. See player.h for rationale. One outstanding command per instance is
 * enforced by blocking API; for two producers add a mutex, not per-command
 * bits.
 * - State functions never block on queue, they only check pending and do work.
 * - All state transitions call on_state() if set. All errors call on_error()
 *   then on_state(UNCONFIGURED).
 * - buf / dec / i2s are borrowed via player_hw_t, created once in main.
 *   Player never creates / destroys them, only buf_flush / dec_reset /
 *   i2s_disable. player_destroy() cleans only queue + event group.
 * - tmp_in / tmp_pcm / dec_pending are static in PSRAM, EXT_RAM_BSS_ATTR,
 *   no malloc in playback loop.
 * - do_reset() = buf_flush + dec_reset + i2s_disable + memset PSRAM tmp +
 *   clear flags. It does NOT touch play_cfg. Only transitions to
 *   UNCONFIGURED clear play_cfg.
 * - STOP -> IDLE keeps play_cfg, PLAY from IDLE needs no CONFIG.
 *   RESET / EOF / ERROR -> UNCONFIGURED clear play_cfg, CONFIG is required.
 */

#include "player.h"
#include "buf.h"
#include "esp_attr.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/idf_additions.h>
#include <freertos/projdefs.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <portmacro.h>
#include <string.h>

static const char *TAG = "player";
#define PREBUFFER_BYTES 8192
#define DIAG_LOG_EVERY 50

static EXT_RAM_BSS_ATTR uint8_t tmp_in[65536];
static EXT_RAM_BSS_ATTR uint8_t tmp_pcm[65536];
static EXT_RAM_BSS_ATTR uint8_t dec_pending[65536];

/**
 * @brief Debug logs in player task.
 * @details 1 = ESP_LOGI on, 0 = compiled out. Keep 1 during debug, 0 in
 * release: ESP_LOGI uses ~1-2 KB stack for vsnprintf, inflates HWM measurement.
 *          PLAYER_LOGI is used in task, not ESP_LOGI directly.
 */
#define PLAYER_DEBUG 1
#if PLAYER_DEBUG
#define PLAYER_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define PLAYER_LOGI(...)                                                       \
  do {                                                                         \
  } while (0)
#endif

/**
 * @brief Command types, app -> player_task via cmd_q.
 * @details Values are internal, not exposed in player.h. cmd_t carries seq
 *          for debugging (not used for ACK matching, ACK is shared).
 */
typedef enum {
  CMD_PLAY = 0, /*!< when IDLE to PLAYING - start PLAYING*/
  CMD_PAUSE,    /*!< when PLAYING to PAUSED - keep PLAYING*/
  CMD_STOP,   /*!< when PLAYING/PAUSED to IDLE - clear buf/i2s keep dec config*/
  CMD_RESUME, /*!< when PAUSED to PLAYING*/
  CMD_CONFIG, /*!< when UNCONFIGURED to CONFIGURING*/
  CMD_EXIT,   /*!< when ANY to EXIT*/
  CMD_RESET,  /*!< when IDLE to UNCONFIGURED - clear buf/i2s dma/dec*/
  CMD_EOF,    /*!< when PLAYING to UNCONFIGURED */
} cmd_type_t;

/**
 * @brief Command struct, queued to player_task.
 * @details seq is incremented per command, for log correlation.
 *          dec_cfg is valid only for CMD_CONFIG, copied to pending_cfg
 *          in player_task(), then to play_cfg in state_configuring().
 */
typedef struct {
  cmd_type_t type;
  uint32_t seq;
  player_dec_cfg_t dec_cfg;
} cmd_t;

/**
 * @brief Pending request, set by command validation, consumed by state_*().
 * @details Single pending slot. If a command arrives while pending is already
 *          set, it is overwritten only if validation in player_task() allows
 * it. States clear pending to PEND_NONE when they consume it. PEND_EXIT is
 * terminal, loop exits after current state.
 */
typedef enum {
  PEND_NONE = 0,
  PEND_PLAY,
  PEND_RESUME,
  PEND_STOP,
  PEND_PAUSE,
  PEND_CONFIG,
  PEND_EXIT,
  PEND_EOF,
  PEND_RESET,
} pend_t;

/**
 * @brief Private player struct, lives inside player_storage_t.
 * @details All fields are inside storage. buf_in / buf_out / dec / dac
 *          are borrowed handles from player_hw_t, data lives outside
 *          storage, buf data in PSRAM, dec internals in dec.c.
 *          task_stack is PLAYER_STACK_SIZE words = 32 KB, the largest field.
 */
struct player_s {
  player_cfg_t cfg;
  pend_t pending;
  player_state_t state;
  // TODO: add dec, i2s, buf in/out
  buf_t buf_in;
  buf_t buf_out;
  dec_t dec;
  i2s_t dac;
  player_dec_cfg_t play_cfg;
  player_dec_cfg_t pending_cfg;
  //
  bool prebuffer_done;
  bool i2s_configured;
  uint32_t last_rate;
  uint8_t last_bits;
  uint8_t last_channels;
  size_t pending_len;
  bool eof_requested;
  int no_progress;
  bool need_i2s_setup;
  bool i2s_enabled;
  //
  uint32_t diag_frames;
  uint32_t diag_pcm_bytes;
  uint32_t diag_i2s_bytes;
  uint32_t diag_last_log;
  //
  TaskHandle_t task_handle;
  StaticTask_t task_tcb;
  StackType_t task_stack[PLAYER_STACK_SIZE];
  //
  uint32_t seq;
  uint8_t queue_buf[PLAYER_QUEUE_LEN * sizeof(cmd_t)];
  QueueHandle_t cmd_q;
  StaticQueue_t queue;
  //
  EventGroupHandle_t ack_group;
  StaticEventGroup_t event_group;
};

/**
 * @brief Compile-time guard: storage must hold whole struct.
 * @details If this fails, increase PLAYER_STORAGE_SIZE in player.h.
 *          PLAYER_STORAGE_SIZE = PLAYER_STACK_SIZE*4 + 1024.
 */
_Static_assert(sizeof(struct player_s) <= sizeof(player_storage_t),
               "PLAYER_STORAGE_SIZE too small");

/**
 * @brief ACK EventGroup bits, shared for all commands.
 * @details ACK_OK = command accepted and completed, ACK_FAIL = rejected (wrong
 * state) or failed, ACK_BUSY = busy, retry. API waits for
 * ACK_OK|ACK_FAIL|ACK_BUSY, clears them on entry (ClearBits) and on exit
 * (pdTRUE in WaitBits). Do NOT add per-command bits, see file header rationale.
 */
#define ACK_OK (1U << 0)   /*!< Command completed */
#define ACK_FAIL (1U << 1) /*!< Command rejected / failed */
#define ACK_BUSY (1U << 2) /*!< Player busy */

// PLAYER TASK

/**
 * @brief Wspólny reset: buf + dec + i2s + statyczne bufory w PSRAM + flagi.
 * @details Nie rusza state ani pending - to ustawia wołający.
 *          Nie rusza play_cfg - play_cfg czyści tylko przejście do
 *          UNCONFIGURED (RESET / EOF / ERROR). STOP tego NIE robi.
 *          Nie rusza pending_cfg - w CONFIGURING jest potrzebny do play_cfg.
 *          PAUSE / RESUME tego NIE wołają, tam pending_len + dec_pending mają
 * zostać.
 */
static void do_reset(player_t play) {
  buf_flush(play->buf_in);
  buf_flush(play->buf_out);
  dec_reset(play->dec);
  i2s_disable(play->dac);
  // [USUNIĘTE] memset(tmp_in...), memset(tmp_pcm...), memset(dec_pending...),
  // memset(tmp_pcm32...)
  play->pending_len = 0;
  play->eof_requested = false;
  play->no_progress = 0;
  play->prebuffer_done = false;
  play->i2s_configured = false;
  play->last_rate = 0;
  play->last_bits = 0;
  play->last_channels = 0;
  play->need_i2s_setup = false;
  play->i2s_enabled = false;
  // [NOWE]
  play->diag_frames = 0;
  play->diag_pcm_bytes = 0;
  play->diag_i2s_bytes = 0;
  play->diag_last_log = 0;
}

/**
 * @brief Drain buf_out to I2S until empty.
 * @details Called from state_playing() each loop and from EOF drain.
 *          Does not touch buf_in, dec_pending or flags.
 */
static void drain_to_dac(player_t play) {
  while (buf_get_fill(play->buf_out) > 0) {
    size_t item_size = 0;
    void *item = buf_read_up_to(play->buf_out, &item_size, 4096);
    if (!item || item_size == 0)
      break;
    size_t off = 0;
    while (off < item_size) {
      size_t written = 0;
      if (i2s_write(play->dac, (uint8_t *)item + off, item_size - off,
                    &written) != I2S_OK)
        break;
      if (written == 0)
        break;
      off += written;
      play->diag_i2s_bytes += written; // [NOWE]
    }
    buf_return_item(play->buf_out, item);
  }
}

static void log_playing_diag(player_t play, const char *tag) {
  size_t in_fill = buf_get_fill(play->buf_in);
  size_t in_free = buf_get_free(play->buf_in);
  size_t out_fill = buf_get_fill(play->buf_out);
  size_t out_free = buf_get_free(play->buf_out);
  dec_info_t info = dec_get_info(play->dec);
  PLAYER_LOGI(
      "[DIAG %s] frames=%u pcm=%u i2s=%u dec_rate=%lu dec_bits=%u dec_ch=%u "
      "buf_in=%u/%u buf_out=%u/%u prebuf=%d no_prog=%d eof=%d",
      tag, (unsigned)play->diag_frames, (unsigned)play->diag_pcm_bytes,
      (unsigned)play->diag_i2s_bytes, (unsigned long)info.sample_rate,
      info.bits, info.channels, (unsigned)in_fill, (unsigned)in_free,
      (unsigned)out_fill, (unsigned)out_free, play->prebuffer_done,
      play->no_progress, play->eof_requested);
}

/**
 * @brief State UNCONFIGURED: wait for CONFIG or EXIT.
 * @details Consumes PEND_CONFIG -> CONFIGURING + on_state(CONFIGURING).
 *          PEND_EXIT is handled by task loop, here just return OK.
 *          Otherwise clears pending to NONE.
 * @param[in] play Handle.
 * @return PLAYER_OK always (no work in this state).
 */
static player_err_t state_unconfigured(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  if (play->pending == PEND_CONFIG) {
    play->pending = PEND_NONE;
    play->state = PLAYER_STATE_CONFIGURING;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_CONFIGURING);
    }
    return PLAYER_OK;
  }
  play->pending = PEND_NONE;
  return PLAYER_OK;
}

/**
 * @brief State IDLE: wait for RESET / PLAY / EXIT.
 * @details PEND_RESET -> do_reset + clear play_cfg -> UNCONFIGURED + ACK_OK,
 *          PEND_PLAY -> PLAYING + ACK_OK, no CONFIG here.
 *          CONFIG is only from UNCONFIGURED, see state_unconfigured().
 *          PEND_EXIT returns OK, loop will exit.
 */
static player_err_t state_idle(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  if (play->pending == PEND_RESET) {
    // TODO: RESET operation
    do_reset(play);
    memset(&play->play_cfg, 0, sizeof(play->play_cfg));

    play->state = PLAYER_STATE_UNCONFIGURED;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_UNCONFIGURED);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_PLAY) {
    play->state = PLAYER_STATE_PLAYING;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_PLAYING);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  play->pending = PEND_NONE;
  return PLAYER_OK;
}

/**
 * @brief State PLAYING: handle STOP/PAUSE/EOF/EXIT, else do playback.
 * @details PEND_STOP -> do_reset, keep play_cfg -> IDLE + ACK_OK,
 *          PEND_PAUSE -> PAUSED + ACK_OK, keeps buf / dec_pending,
 *          PEND_EOF -> eof_requested = true, drain happens below,
 *          PEND_EXIT -> return OK, loop exits.
 *          EOF drain: when eof_requested and buf_in empty and buf_out empty,
 *          drop leftover pending_len (incomplete frame), drain_to_dac(),
 *          write 4608 zero bytes, delay 50 ms, do_reset + clear play_cfg
 *          -> UNCONFIGURED + ACK_OK.
 *          Playback step: pending + buf_in -> tmp_in -> dec_decode ->
 *          tmp_pcm -> buf_out -> drain_to_dac(). On first decoded frame
 *          retune I2S to decoded rate/bits/ch. 16-bit PCM is -6dB (/2)
 *          for test, 24-bit is next step.
 *          On underrun without EOF, delay 1 ms and retry, not EOF.
 */
static player_err_t state_playing(player_t play) {
  if (play->pending == PEND_EXIT)
    return PLAYER_OK;
  if (play->pending == PEND_STOP) {
    log_playing_diag(play, "STOP"); // [NOWE]
    do_reset(play);
    play->state = PLAYER_STATE_IDLE;
    play->pending = PEND_NONE;
    if (play->cfg.on_state)
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_IDLE);
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_PAUSE) {
    log_playing_diag(play, "PAUSE"); // [NOWE]
    play->state = PLAYER_STATE_PAUSED;
    play->pending = PEND_NONE;
    if (play->cfg.on_state)
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_PAUSED);
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_EOF) {
    play->eof_requested = true;
    play->pending = PEND_NONE;
  }

  if (!play->prebuffer_done) {
    size_t avail = buf_get_fill(play->buf_in) + play->pending_len;
    if (avail < PREBUFFER_BYTES && !play->eof_requested) {
      vTaskDelay(pdMS_TO_TICKS(5));
      return PLAYER_OK;
    }
    play->prebuffer_done = true;
    PLAYER_LOGI("[DIAG PREBUFFER] done avail=%u", (unsigned)avail); // [NOWE]
  }

  if (play->eof_requested && buf_get_fill(play->buf_in) == 0 &&
      play->pending_len == 0 && buf_get_fill(play->buf_out) == 0) {
    log_playing_diag(play, "EOF"); // [NOWE] było: od razu do_reset bez logu
    if (play->i2s_configured) {
      memset(tmp_pcm, 0, 4608);
      size_t written = 0;
      i2s_write(play->dac, tmp_pcm, 4608, &written);
      play->diag_i2s_bytes += written; // [NOWE]
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    do_reset(play);
    memset(&play->play_cfg, 0, sizeof(play->play_cfg));
    play->state = PLAYER_STATE_UNCONFIGURED;
    if (play->cfg.on_state)
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_UNCONFIGURED);
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }

  // [NOWE] cała gałąź PCM, wcześniej nie było, PCM leciał przez dec_decode
  if (play->play_cfg.codec == DEC_CODEC_PCM) {
    if (!play->i2s_configured) {
      if (i2s_reconfig(play->dac, play->play_cfg.sample_rate,
                       play->play_cfg.bits,
                       play->play_cfg.channels) != I2S_OK) {
        return PLAYER_ERR_FAIL_I2S;
      }
      play->last_rate = play->play_cfg.sample_rate;
      play->last_bits = play->play_cfg.bits;
      play->last_channels = play->play_cfg.channels;
      play->i2s_configured = true;
      i2s_enable(play->dac);
      PLAYER_LOGI("i2s reconfig PCM: %lu Hz %u bit %u ch",
                  (unsigned long)play->last_rate, play->last_bits,
                  play->last_channels);
    }
    size_t avail = buf_get_fill(play->buf_in);
    if (avail == 0) {
      if (!play->eof_requested)
        vTaskDelay(pdMS_TO_TICKS(2));
      return PLAYER_OK;
    }
    size_t want = avail < sizeof(tmp_pcm) ? avail : sizeof(tmp_pcm);
    size_t n = buf_read_copy(play->buf_in, tmp_pcm, want);
    if (n == 0)
      return PLAYER_OK;
    if (play->play_cfg.bits == 16) {
      int16_t *pcm = (int16_t *)tmp_pcm;
      for (size_t i = 0; i < n / 2; i++)
        pcm[i] = (int16_t)((int32_t)pcm[i] / 2);
    }
    size_t off = 0;
    while (off < n) {
      size_t w = buf_write_partial(play->buf_out, tmp_pcm + off, n - off);
      if (w == 0) {
        drain_to_dac(play);
        vTaskDelay(pdMS_TO_TICKS(1));
      } else
        off += w;
    }
    play->diag_frames++;
    play->diag_pcm_bytes += n;
    play->no_progress = 0;
    drain_to_dac(play);
    if (play->diag_frames - play->diag_last_log >= DIAG_LOG_EVERY) {
      play->diag_last_log = play->diag_frames;
      log_playing_diag(play, "PLAY PCM");
    }
    return PLAYER_OK;
  }

  // MP3 / AAC, bez zmian poza diagnostyką
  size_t total_in = play->pending_len;
  if (play->pending_len > 0)
    memcpy(tmp_in, dec_pending, play->pending_len);
  while (total_in < sizeof(tmp_in) && buf_get_fill(play->buf_in) > 0) {
    size_t n = buf_read_copy(play->buf_in, tmp_in + total_in,
                             sizeof(tmp_in) - total_in);
    if (n == 0)
      break;
    total_in += n;
  }

  bool need_more_data = false;
  if (total_in > 0) {
    size_t consumed = 0, produced = 0;
    dec_err_t rc = dec_decode(play->dec, tmp_in, total_in, tmp_pcm,
                              sizeof(tmp_pcm), &consumed, &produced);
    if (rc != DEC_OK) {
      if (play->eof_requested && buf_get_fill(play->buf_in) == 0) {
        play->pending_len = 0;
        return PLAYER_OK;
      }
      log_playing_diag(play, "FAIL_DEC"); // [NOWE]
      return PLAYER_ERR_FAIL_DEC;
    }
    if (consumed < total_in) {
      size_t left = total_in - consumed;
      memmove(dec_pending, tmp_in + consumed, left);
      play->pending_len = left;
    } else
      play->pending_len = 0;

    if (produced > 0) {
      play->no_progress = 0;
      play->diag_frames++;              // [NOWE]
      play->diag_pcm_bytes += produced; // [NOWE]
      dec_info_t info = dec_get_info(play->dec);
      uint32_t rate =
          info.sample_rate ? info.sample_rate : play->play_cfg.sample_rate;
      uint8_t bits = info.bits ? info.bits : play->play_cfg.bits;
      uint8_t ch = info.channels ? info.channels : play->play_cfg.channels;
      if (rate != 0 && bits != 0 && ch != 0) {
        if (!play->i2s_configured || rate != play->last_rate ||
            bits != play->last_bits || ch != play->last_channels) {
          if (i2s_reconfig(play->dac, rate, bits, ch) != I2S_OK)
            return PLAYER_ERR_FAIL_I2S;
          play->last_rate = rate;
          play->last_bits = bits;
          play->last_channels = ch;
          play->i2s_configured = true;
          i2s_enable(play->dac);
          PLAYER_LOGI("i2s reconfig: %lu Hz %u bit %u ch", (unsigned long)rate,
                      bits, ch);
        }
      }
      if (bits == 16) {
        int16_t *pcm = (int16_t *)tmp_pcm;
        for (size_t i = 0; i < produced / 2; i++)
          pcm[i] = (int16_t)((int32_t)pcm[i] / 2);
      }
      size_t off = 0;
      while (off < produced) {
        size_t w =
            buf_write_partial(play->buf_out, tmp_pcm + off, produced - off);
        if (w == 0) {
          drain_to_dac(play);
          vTaskDelay(pdMS_TO_TICKS(1));
        } else
          off += w;
      }
      if (play->diag_frames - play->diag_last_log >= DIAG_LOG_EVERY) { // [NOWE]
        play->diag_last_log = play->diag_frames;
        log_playing_diag(play, "PLAY");
      }
    } else if (consumed == 0) {
      if (play->eof_requested && buf_get_fill(play->buf_in) == 0) {
        play->pending_len = 0;
      } else if (total_in < sizeof(tmp_in)) {
        need_more_data = true;
      } else {
        play->no_progress++;
        if (play->no_progress % 5 == 0)
          log_playing_diag(play, "NO_PROGRESS"); // [NOWE]
      }
    }
  } else
    need_more_data = true;

  drain_to_dac(play);
  if (play->no_progress > 10) {
    log_playing_diag(play, "FAIL_NO_PROGRESS"); // [NOWE]
    return PLAYER_ERR_FAIL_DEC;
  }
  if (need_more_data && !play->eof_requested)
    vTaskDelay(pdMS_TO_TICKS(2));
  return PLAYER_OK;
}

/**
 * @brief State PAUSED: wait for STOP / RESUME / EXIT.
 * @details PEND_STOP -> do_reset, keep play_cfg -> IDLE + ACK_OK,
 *          PEND_RESUME -> PLAYING + ACK_OK. No playback in PAUSED,
 *          no dec_reset, no buf_flush, buf / dec_pending stay.
 *          Task blocks on queue (portMAX_DELAY in player_task()).
 */
static player_err_t state_paused(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  if (play->pending == PEND_STOP) {
    do_reset(play);

    play->state = PLAYER_STATE_IDLE;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_IDLE);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_RESUME) {
    play->state = PLAYER_STATE_PLAYING;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_PLAYING);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  play->pending = PEND_NONE;
  return PLAYER_OK;
}

/**
 * @brief State CONFIGURING: set codec, reset, then IDLE.
 * @details PEND_EXIT returns OK, loop exits.
 *          Otherwise: dec_set_codec(pending_cfg.codec), on fail ACK_FAIL
 *          + return FAIL_DEC -> state_error(). On success do_reset(),
 *          play_cfg = pending_cfg, clear pending_cfg -> IDLE + ACK_OK
 *          + on_state(IDLE). No malloc here, buf / dec / i2s are borrowed.
 *          i2s_configured = false, real I2S config after first decoded
 *          frame in state_playing().
 */
static player_err_t state_configuring(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }

  if (dec_set_codec(play->dec, play->pending_cfg.codec) != DEC_OK) {
    xEventGroupSetBits(play->ack_group, ACK_FAIL);
    return PLAYER_ERR_FAIL_DEC;
  }

  do_reset(play);
  play->play_cfg = play->pending_cfg;
  memset(&play->pending_cfg, 0, sizeof(play->pending_cfg));

  play->state = PLAYER_STATE_IDLE;
  play->pending = PEND_NONE;
  if (play->cfg.on_state) {
    play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_IDLE);
  }
  xEventGroupSetBits(play->ack_group, ACK_OK);
  return PLAYER_OK;
}

/**
 * @brief Error handler: safe state on any state_*() error.
 * @details Sets state UNCONFIGURED, clears pending, calls on_error(err),
 *          then on_state(UNCONFIGURED). Does do_reset() + clear play_cfg.
 *          Does NOT dec_deinit / i2s_del, handles are borrowed.
 *          If err == PLAYER_OK, does nothing.
 */
static player_err_t state_error(player_t play, player_err_t err) {
  if (err == PLAYER_OK) {
    return PLAYER_OK;
  }
  // Do RESET
  do_reset(play);
  memset(&play->play_cfg, 0, sizeof(play->play_cfg));

  play->state = PLAYER_STATE_UNCONFIGURED;
  play->pending = PEND_NONE;
  if (play->cfg.on_error) {
    play->cfg.on_error(play->cfg.user_ctx, err);
  }
  if (play->cfg.on_state) {
    play->cfg.on_state(play->cfg.user_ctx, play->state);
  }
  return PLAYER_OK;
}

/**
 * @brief Task cleanup on EXIT.
 * @details Called once before vTaskDelete(). Nothing to free here,
 *          buf / dec / i2s are borrowed and live in main.
 */
static player_err_t state_cleanup(player_t play) { return PLAYER_OK; }

/**
 * @brief Player task, borrower of buf/dec/i2s and owner of state machine.
 * @details Loop: 1) wait_q = portMAX_DELAY in IDLE/UNCONFIGURED/PAUSED,
 *             0 in PLAYING/CONFIGURING. 2) xQueueReceive() command,
 *             validate vs current state: wrong state -> ACK_FAIL
 *             immediately, correct -> set pending. 3) Dispatch to
 *             state_*(), capture err. 4) If err != OK, state_error()
 *             -> UNCONFIGURED. 5) If pending == PEND_EXIT, exit loop,
 *             cleanup, ACK_OK, vTaskDelete.
 */
static void player_task(void *arg) {
  PLAYER_LOGI("task startup");

  player_t play = (player_t)arg;
  play->state = PLAYER_STATE_UNCONFIGURED;

  cmd_t cmd;
  player_err_t err = PLAYER_OK;

  while (play->pending != PEND_EXIT) {
    TickType_t wait_q = (play->state == PLAYER_STATE_IDLE ||
                         play->state == PLAYER_STATE_UNCONFIGURED ||
                         play->state == PLAYER_STATE_PAUSED)
                            ? portMAX_DELAY
                            : 0;
    if (xQueueReceive(play->cmd_q, &cmd, wait_q) == pdTRUE) {
      switch (cmd.type) {
      case CMD_EXIT:
        play->pending = PEND_EXIT;
        break;
      case CMD_CONFIG:
        if (play->state != PLAYER_STATE_UNCONFIGURED) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_CONFIG;
          // TODO: assign values to config
          play->pending_cfg = cmd.dec_cfg;
        }
        break;
      case CMD_RESET:
        if (play->state != PLAYER_STATE_IDLE) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_RESET;
        }
        break;
      case CMD_PLAY:
        if (play->state != PLAYER_STATE_IDLE) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_PLAY;
        }
        break;
      case CMD_STOP:
        if (play->state != PLAYER_STATE_PLAYING &&
            play->state != PLAYER_STATE_PAUSED) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_STOP;
        }
        break;
      case CMD_RESUME:
        if (play->state != PLAYER_STATE_PAUSED) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_RESUME;
        }
        break;
      case CMD_PAUSE:
        if (play->state != PLAYER_STATE_PLAYING) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_PAUSE;
        }
        break;
      case CMD_EOF:
        if (play->state != PLAYER_STATE_PLAYING) {
          xEventGroupSetBits(play->ack_group, ACK_FAIL);
        } else {
          play->pending = PEND_EOF;
        }
        break;
      default:
        break;
      }
    }

    // State machine
    switch (play->state) {
    case PLAYER_STATE_UNCONFIGURED:
      err = state_unconfigured(play);
      break;
    case PLAYER_STATE_IDLE:
      err = state_idle(play);
      break;
    case PLAYER_STATE_PAUSED:
      err = state_paused(play);
      break;
    case PLAYER_STATE_CONFIGURING:
      err = state_configuring(play);
      break;
    case PLAYER_STATE_PLAYING:
      err = state_playing(play);
      break;
    default:
      break;
    }

    if (err != PLAYER_OK) {
      // TODO: error handler
      state_error(play, err);
    }
  }
  PLAYER_LOGI("task cleanup");
  state_cleanup(play);
  xEventGroupSetBits(play->ack_group, ACK_OK);
  vTaskDelete(NULL);
}

player_err_t player_init(player_storage_t *storage, const player_cfg_t *cfg,
                         const player_hw_t *hw, player_t *out) {

  if (storage == NULL || cfg == NULL || hw == NULL || out == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  if (hw->buf_in == NULL || hw->buf_out == NULL || hw->i2s == NULL ||
      hw->dec == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  player_t play = (player_t)storage;
  memset(play, 0, sizeof(*play));

  play->cfg = *cfg;
  play->cfg.stack_size = PLAYER_STACK_SIZE;
  if (play->cfg.priority == 0) {
    play->cfg.priority = 5;
  }
  if (play->cfg.core_id < 0 || play->cfg.core_id > 1) {
    play->cfg.core_id = 1;
  }

  play->buf_in = hw->buf_in;
  play->buf_out = hw->buf_out;
  play->dac = hw->i2s;
  play->dec = hw->dec;

  play->cmd_q = xQueueCreateStatic(PLAYER_QUEUE_LEN, sizeof(cmd_t),
                                   play->queue_buf, &play->queue);
  play->ack_group = xEventGroupCreateStatic(&play->event_group);
  if (play->cmd_q == NULL || play->ack_group == NULL) {
    return PLAYER_ERR_NO_MEM;
  }
  play->state = PLAYER_STATE_UNCONFIGURED;
  *out = play;

  ESP_LOGI(TAG,
           "PSRAM: tmp_in %p ext=%d tmp_pcm %p ext=%d dec_pending %p ext=%d",
           tmp_in, esp_ptr_external_ram(tmp_in), tmp_pcm,
           esp_ptr_external_ram(tmp_pcm), dec_pending,
           esp_ptr_external_ram(dec_pending));
  ESP_LOGI(TAG, "heap internal free=%u PSRAM free=%u",
           heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  return PLAYER_OK;
}

player_err_t player_destroy(player_t play) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  if (play->task_handle != NULL) {
    return PLAYER_ERR_FAIL;
  }
  vQueueDelete(play->cmd_q);
  vEventGroupDelete(play->ack_group);
  return PLAYER_OK;
}

player_err_t player_start(player_t play) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  if (play->task_handle != NULL) {
    return PLAYER_ERR_FAIL;
  }
  play->task_handle = xTaskCreateStaticPinnedToCore(
      player_task, "player", PLAYER_STACK_SIZE, play, play->cfg.priority,
      play->task_stack, &play->task_tcb, play->cfg.core_id);
  if (play->task_handle == NULL) {
    return PLAYER_ERR_NO_MEM;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_exit(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_EXIT, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  play->task_handle = NULL;
  return PLAYER_OK;
}

player_err_t player_cmd_play(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_PLAY, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_stop(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_STOP, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_pause(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_PAUSE, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_resume(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_RESUME, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_config(player_t play, const player_dec_cfg_t *dec_cfg,
                               uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  // TODO: decoder config validation
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_CONFIG, .seq = ++play->seq, .dec_cfg = *dec_cfg};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_reset(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_RESET, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_cmd_eof(player_t play, uint32_t timeout_ms) {
  if (play == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY);

  cmd_t cmd = {.type = CMD_EOF, .seq = ++play->seq};
  xQueueSend(play->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(play->ack_group, ACK_FAIL | ACK_OK | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return PLAYER_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return PLAYER_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return PLAYER_ERR_BUSY;
  }
  return PLAYER_OK;
}

player_err_t player_get_diag(player_t play, player_diag_t *out) {
  if (play == NULL || out == NULL) {
    return PLAYER_ERR_INVALID_ARG;
  }
  out->heap_free = xPortGetFreeHeapSize();
  out->heap_min = xPortGetMinimumEverFreeHeapSize();
  out->state = play->state;
  if (play->task_handle != NULL) {
    out->stack_hwm = uxTaskGetStackHighWaterMark(play->task_handle);
  } else {
    out->stack_hwm = 0;
  }
  out->frames = play->diag_frames;       // [NOWE]
  out->pcm_bytes = play->diag_pcm_bytes; // [NOWE]
  out->i2s_bytes = play->diag_i2s_bytes; // [NOWE]
  return PLAYER_OK;
}

const char *player_err_to_str(player_err_t err) {
  switch (err) {
  case PLAYER_OK:
    return "OK";
  case PLAYER_ERR_INVALID_ARG:
    return "INVALID_ARG";
  case PLAYER_ERR_NO_MEM:
    return "NO_MEM";
  case PLAYER_ERR_TIMEOUT:
    return "TIMEOUT";
  case PLAYER_ERR_BUSY:
    return "BUSY";
  case PLAYER_ERR_FAIL:
    return "FAIL";
  case PLAYER_ERR_FAIL_I2S:
    return "FAIL_I2S";
  case PLAYER_ERR_FAIL_DEC:
    return "FAIL_DEC";
  default:
    return "FAIL";
  }
}
