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

static EXT_RAM_BSS_ATTR uint8_t tmp_in[16384];
static EXT_RAM_BSS_ATTR uint8_t tmp_pcm[32768];
static EXT_RAM_BSS_ATTR uint8_t dec_pending[16384];

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
 * @todo Add play_config_t codec / sample_rate / bits / channels to CMD_CONFIG.
 *       For now player_cmd_config() carries only timeout.
 */
typedef struct {
  cmd_type_t type;
  uint32_t seq;
  // TODO: dec codec, bits, channels
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
 * @details All fields are inside storage, except buf_in / buf_out data
 *          (PSRAM, malloc in CONFIGURING) and decoder internals.
 *          task_stack is PLAYER_STACK_SIZE words = 32 KB, the largest field.
 * @todo Add dec*, i2s_handle, buf_in*, buf_out*, sample_rate/bits/channels,
 *       prebuffer_done, i2s_configured, i2s_started. Only pointers here,
 *       data in PSRAM, not in storage.
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

// TODO: do_reset(player_t play), do_drain(player_t play)

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
    }
    buf_return_item(play->buf_out, item);
  }
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
 * @brief State IDLE: wait for CONFIG / RESET / PLAY / EXIT.
 * @details PEND_CONFIG -> CONFIGURING, PEND_RESET -> UNCONFIGURED + ACK_OK,
 *          PEND_PLAY -> PLAYING + ACK_OK. RESET here only changes state,
 *          real buf/dec/i2s clear is TODO in state_idle / state_configuring.
 *          PEND_EXIT returns OK, loop will exit.
 * @param[in] play Handle.
 * @return PLAYER_OK.
 */
static player_err_t state_idle(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  } /*
   if (play->pending == PEND_CONFIG) {
     play->state = PLAYER_STATE_CONFIGURING;
     play->pending = PEND_NONE;
     if (play->cfg.on_state) {
       play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_CONFIGURING);
     }
     return PLAYER_OK;
   }*/
  if (play->pending == PEND_RESET) {
    // TODO: RESET operation
    buf_flush(play->buf_in);
    buf_flush(play->buf_out);
    dec_reset(play->dec);
    i2s_disable(play->dac);
    play->pending_len = 0;
    play->eof_requested = false;
    play->no_progress = 0;
    play->prebuffer_done = false;
    play->i2s_configured = false;
    play->last_rate = 0;
    play->last_bits = 0;
    play->last_channels = 0;
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
 * @details PEND_STOP -> IDLE + ACK_OK (TODO: flush buf),
 *          PEND_PAUSE -> PAUSED + ACK_OK,
 *          PEND_EOF -> drain + UNCONFIGURED + ACK_OK (TODO: drain >=249 logic),
 *          PEND_EXIT -> return OK, loop exits.
 *          Otherwise playback logic (TODO):
 *          1) prebuffer: fill buf_in to threshold before first i2s_write,
 *          2) decode first frame, if codec, retune I2S to decoded rate/bits/ch,
 *          3) loop src -> buf_in -> dec -> buf_out -> i2s_write,
 *          4) on underrun, go back to prebuffer, not EOF.
 * @param[in] play Handle.
 * @return PLAYER_OK, or dec/i2s error -> state_error() in task loop.
 */
static player_err_t state_playing(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  if (play->pending == PEND_STOP) {
    // TODO: Flush buffers
    buf_flush(play->buf_in);
    buf_flush(play->buf_out);
    dec_reset(play->dec);
    i2s_disable(play->dac);
    play->eof_requested = false;
    play->pending_len = 0;
    play->i2s_configured = false;
    play->prebuffer_done = false;
    play->pending_len = 0;
    play->eof_requested = false;

    play->state = PLAYER_STATE_IDLE;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_IDLE);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_PAUSE) {
    play->state = PLAYER_STATE_PAUSED;
    play->pending = PEND_NONE;
    if (play->cfg.on_state) {
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_PAUSED);
    }
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }
  if (play->pending == PEND_EOF) {
    // TODO: Check if all drained, goto unconfigured, ACK_OK

    play->eof_requested = true;
    play->pending = PEND_NONE;
    return PLAYER_OK;
  }

  // 1. EOF drain - jak już nie ma nic do zdekodowania i nic do zagrania
  if (play->eof_requested && buf_get_fill(play->buf_in) == 0 &&
      play->pending_len == 0 && buf_get_fill(play->buf_out) == 0) {
    drain_to_dac(play);
    buf_flush(play->buf_in);
    buf_flush(play->buf_out);
    dec_reset(play->dec);
    i2s_disable(play->dac);
    play->eof_requested = false;
    play->pending_len = 0;
    play->no_progress = 0;
    play->prebuffer_done = false;
    play->i2s_configured = false;
    play->last_rate = 0;
    play->last_bits = 0;
    play->last_channels = 0;
    memset(&play->play_cfg, 0, sizeof(play->play_cfg));
    play->state = PLAYER_STATE_UNCONFIGURED;
    if (play->cfg.on_state)
      play->cfg.on_state(play->cfg.user_ctx, PLAYER_STATE_UNCONFIGURED);
    xEventGroupSetBits(play->ack_group, ACK_OK);
    return PLAYER_OK;
  }

  // 2. Jeden krok dekodowania: pending + buf_in -> tmp_in -> dec_decode
  size_t fill_in = buf_get_fill(play->buf_in);
  if (fill_in > 0 || play->pending_len > 0) {
    size_t total_in = play->pending_len;
    memcpy(tmp_in, dec_pending, play->pending_len);
    if (fill_in > 0) {
      size_t item_size = 0;
      void *item = buf_read_up_to(play->buf_in, &item_size,
                                  sizeof(tmp_in) - play->pending_len);
      if (item) {
        memcpy(tmp_in + play->pending_len, item, item_size);
        total_in += item_size;
        buf_return_item(play->buf_in, item);
      }
    }
    if (total_in > 0) {
      size_t consumed = 0, produced = 0;
      dec_err_t rc = dec_decode(play->dec, tmp_in, total_in, tmp_pcm,
                                sizeof(tmp_pcm), &consumed, &produced);
      if (rc != DEC_OK)
        return PLAYER_ERR_FAIL_DEC;

      if (consumed < total_in) {
        size_t left = total_in - consumed;
        memmove(dec_pending, tmp_in + consumed, left);
        play->pending_len = left;
      } else {
        play->pending_len = 0;
      }

      if (produced > 0) {
        dec_info_t info = dec_get_info(play->dec);
        if (info.sample_rate != 0 && info.bits != 0 && info.channels != 0) {
          if (!play->i2s_configured || info.sample_rate != play->last_rate ||
              info.bits != play->last_bits ||
              info.channels != play->last_channels) {
            if (i2s_reconfig(play->dac, info.sample_rate, info.bits,
                             info.channels) == I2S_OK) {
              play->last_rate = info.sample_rate;
              play->last_bits = info.bits;
              play->last_channels = info.channels;
              play->i2s_configured = true;
              i2s_enable(play->dac);
              PLAYER_LOGI("i2s reconfig: %lu Hz %u bit %u ch",
                          (unsigned long)play->last_rate, play->last_bits,
                          play->last_channels);
            }
          }
        }
        // 16-bit, -6dB jak w teście, 24-bit robimy w następnym kroku
        int16_t *pcm = (int16_t *)tmp_pcm;
        size_t samples = produced / 2;
        for (size_t i = 0; i < samples; i++) {
          int32_t s = pcm[i];
          pcm[i] = (int16_t)(s / 2);
        }
        play->no_progress = 0;
        size_t off = 0;
        while (off < produced) {
          size_t w =
              buf_write_partial(play->buf_out, tmp_pcm + off, produced - off);
          if (w == 0) {
            drain_to_dac(play);
            vTaskDelay(pdMS_TO_TICKS(1));
          } else {
            off += w;
          }
        }
      } else {
        if (consumed == 0)
          play->no_progress++;
        else
          play->no_progress = 0;
      }
    }
  }

  // 3. Drain
  drain_to_dac(play);

  if (play->no_progress > 100)
    return PLAYER_ERR_FAIL_DEC;
  if (fill_in == 0 && play->pending_len == 0 && !play->eof_requested) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  return PLAYER_OK;
}

/**
 * @brief State PAUSED: wait for STOP / RESUME / EXIT.
 * @details PEND_STOP -> IDLE + ACK_OK (TODO: flush buf),
 *          PEND_RESUME -> PLAYING + ACK_OK. No playback in PAUSED,
 *          task blocks on queue (portMAX_DELAY in player_task()).
 * @param[in] play Handle.
 * @return PLAYER_OK.
 */
static player_err_t state_paused(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  if (play->pending == PEND_STOP) {
    // TODO: Flush buffers
    buf_flush(play->buf_in);
    buf_flush(play->buf_out);
    dec_reset(play->dec);
    i2s_disable(play->dac);
    play->i2s_configured = false;
    play->prebuffer_done = false;
    play->pending_len = 0;
    play->eof_requested = false;
    play->no_progress = 0;

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
 * @brief State CONFIGURING: do buf/dec/i2s setup, then IDLE.
 * @details PEND_EXIT returns OK, loop exits.
 *          Otherwise (TODO): malloc buf_in/out in PSRAM, dec_init(codec),
 *          for PCM create I2S with cfg rate/bits/ch, for codec set
 *          i2s_configured = false, real I2S config after first decoded frame
 *          in state_playing(). On success -> IDLE + ACK_OK + on_state(IDLE).
 *          On fail -> return FAIL_DEC / FAIL_I2S, task loop calls
 * state_error().
 * @param[in] play Handle.
 * @return PLAYER_OK, or FAIL_DEC / FAIL_I2S on setup fail.
 */
static player_err_t state_configuring(player_t play) {
  if (play->pending == PEND_EXIT) {
    return PLAYER_OK;
  }
  // TODO: decoder configuration

  if (dec_set_codec(play->dec, play->pending_cfg.codec) != DEC_OK) {
    xEventGroupSetBits(play->ack_group, ACK_FAIL);
    return PLAYER_ERR_FAIL_DEC;
  }
  dec_reset(play->dec);
  buf_flush(play->buf_in);
  buf_flush(play->buf_out);
  i2s_disable(play->dac);

  play->play_cfg = play->pending_cfg;
  memset(&play->pending_cfg, 0, sizeof(play->pending_cfg));
  play->pending_len = 0;
  play->eof_requested = false;
  play->no_progress = 0;
  play->prebuffer_done = false;
  play->i2s_configured = false;
  play->last_rate = 0;
  play->last_bits = 0;
  play->last_channels = 0;

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
 *          then on_state(UNCONFIGURED). TODO: mute/stop I2S, clear buf,
 *          close source, dec_deinit / i2s_del, then UNCONFIGURED.
 *          If err == PLAYER_OK, does nothing.
 * @param[in] play Handle.
 * @param[in] err Error from state_*(), PLAYER_OK = no error.
 * @return PLAYER_OK always.
 */
static player_err_t state_error(player_t play, player_err_t err) {
  if (err == PLAYER_OK) {
    return PLAYER_OK;
  }
  // Do RESET
  buf_flush(play->buf_in);
  buf_flush(play->buf_out);
  dec_reset(play->dec);
  i2s_disable(play->dac);
  play->eof_requested = false;
  play->pending_len = 0;
  play->no_progress = 0;
  play->prebuffer_done = false;
  play->i2s_configured = false;
  play->last_rate = 0;
  play->last_bits = 0;
  play->last_channels = 0;
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
 * @details Called once before vTaskDelete(). TODO: free buf, dec_deinit,
 * i2s_del.
 * @param[in] play Handle.
 * @return PLAYER_OK.
 */
static player_err_t state_cleanup(player_t play) { return PLAYER_OK; }

/**
 * @brief Player task, owner of buf/dec/i2s (TODO) and state machine.
 * @details Loop:
 *          1) wait_q = portMAX_DELAY in IDLE/UNCONFIGURED/PAUSED (block),
 *             0 in PLAYING/CONFIGURING (poll, do work).
 *          2) xQueueReceive() command, validate vs current state:
 *             wrong state -> ACK_FAIL immediately, correct -> set pending.
 *          3) Dispatch to state_*() based on play->state, capture err.
 *          4) If err != OK, call state_error() -> UNCONFIGURED.
 *          5) If pending == PEND_EXIT, exit loop, cleanup, ACK_OK, vTaskDelete.
 * @param[in] arg player_t, from player_start().
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
