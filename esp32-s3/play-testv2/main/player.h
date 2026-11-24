/**
 * @file player.h
 * @brief SymphonyESP Player - opaque handle, static storage, task +
 * command/ACK.
 *
 * @details Architecture:
 * - One owner per instance. Player owns its task, command queue, ACK event
 * group and, after CONFIGURING, buf_in / buf_out (PSRAM), decoder and I2S.
 * - No globals, no malloc for the player itself. Caller provides
 *   @ref player_storage_t (static .bss) and player_init() casts it to
 *   @ref player_s. Only decoder internals may use heap, everything else
 *   is static (see player.c).
 * - Commands go down: app -> player_task via command queue.
 *   ACKs go up: player_task -> app via EventGroup (ACK_OK / ACK_FAIL /
 * ACK_BUSY).
 * - Errors go up via on_error callback, state changes via on_state callback.
 *   Both callbacks carry user_ctx, so one callback can serve many instances.
 * - Safe state on error: mute/stop, clear buffers, close source, report to app,
 *   go to UNCONFIGURED. See state_error() in player.c.
 * - I2S clock is NOT configured in CONFIGURING for codecs (AAC/MP3/FLAC).
 *   Real sample_rate / bits / channels are known only after first decoded
 * frame. For PCM they are known from config. See state_configuring() TODO.
 *
 * @note This file must be includable first. It includes sdkconfig.h so
 *       FreeRTOS knows CONFIG_IDF_TARGET_ESP32S3 and NUMBER_OF_CORES,
 *       otherwise FreeRTOS.h fails with portYIELD_CORE error on ESP-IDF v6.
 */

#ifndef PLAYER_H
#define PLAYER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "buf.h"
#include "dec.h"
#include "i2s.h"
#include <sdkconfig.h>
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Player task stack size in words (StackType_t).
 * @details 8192 words = 32768 bytes. Measured HWM in skeleton test:
 *          ~6892 words free, ~1300 words used. Keep margin for decoder
 *          and i2s_write call chain. Do not shrink before measuring
 *          with real decode + logs off.
 */
#define PLAYER_STACK_SIZE 8192

/**
 * @brief Command queue length in commands.
 * @details One outstanding command per instance is expected (blocking API).
 *          4 is margin for EXIT + CONFIG + PLAY queued quickly.
 */
#define PLAYER_QUEUE_LEN 4

/**
 * @brief Player storage size in bytes.
 * @details Must hold whole struct player_s: task_tcb + task_stack
 *          (PLAYER_STACK_SIZE * 4) + queue + event group + cfg + state.
 *          +1024 is margin for task_tcb / StaticQueue_t / StaticEventGroup_t
 *          and future dec* / i2s* / buf* pointers (only pointers, not data).
 *          Data of buf_in / buf_out lives in PSRAM, not in storage.
 */
#define PLAYER_STORAGE_SIZE (PLAYER_STACK_SIZE * sizeof(uint32_t) + 1024)

/**
 * @brief Opaque storage for one player instance.
 * @details Caller declares it static, e.g.:
 *          @code static player_storage_t storage1; @endcode
 *          It is just a byte box. player_init() casts it to player_s and
 *          memsets it. Do not read/write _opaque directly.
 */
typedef struct {
  uint32_t _opaque[PLAYER_STORAGE_SIZE / sizeof(uint32_t)];
} player_storage_t;

/**
 * @brief Opaque player handle.
 * @details Points inside caller-provided player_storage_t. Never allocate
 *          with malloc, never dereference from app code, struct player_s
 *          is private to player.c.
 */
typedef struct player_s *player_t;

/**
 * @brief Player error codes.
 * @details All API functions return this. PLAYER_OK = success.
 *          ACK_FAIL from task maps to PLAYER_ERR_FAIL, timeout to TIMEOUT.
 */
typedef enum {
  PLAYER_OK = 0,
  PLAYER_ERR_INVALID_ARG = -1,
  PLAYER_ERR_NO_MEM = -2,
  PLAYER_ERR_TIMEOUT = -3,
  PLAYER_ERR_FAIL = -4,
  PLAYER_ERR_FAIL_I2S = -5,
  PLAYER_ERR_FAIL_DEC = -6,
  PLAYER_ERR_BUSY = -7,
} player_err_t;

/**
 * @brief Player states.
 * @details UNCONFIGURED is the safe state after init / error / RESET.
 *          CONFIGURING is transient, does buf/dec/i2s setup, then IDLE.
 *          PLAYING does src -> buf_in -> dec -> buf_out -> i2s_write.
 *          PAUSED keeps buffers and config, only stops i2s_write.
 */
typedef enum {
  PLAYER_STATE_IDLE = 0,
  PLAYER_STATE_UNCONFIGURED,
  PLAYER_STATE_CONFIGURING,
  PLAYER_STATE_PLAYING,
  PLAYER_STATE_PAUSED,
} player_state_t;

/**
 * @brief Diagnostics snapshot, copy, not live pointer.
 * @details heap_free / heap_min are global heap (for leak check).
 *          stack_hwm is FreeRTOS high-water mark in WORDS FREE, not used.
 *          Used = PLAYER_STACK_SIZE - stack_hwm. Example: HWM 6892 means
 *          used 1300 words = 5200 bytes. state is current player state.
 */
typedef struct {
  size_t heap_free;
  size_t heap_min;
  uint32_t stack_hwm;
  player_state_t state;
} player_diag_t;

/**
 * @brief Callback: player changed state.
 * @param[in] user_ctx Caller context from player_cfg_t, distinguishes
 * instances.
 * @param[in] state New state, see player_state_t.
 * @note Called from player task context. Keep it short, no blocking, no
 * player_cmd_*.
 */
typedef void (*player_on_state_cb_t)(void *user_ctx, player_state_t state);

/**
 * @brief Callback: player hit an error and went to UNCONFIGURED.
 * @param[in] user_ctx Caller context from player_cfg_t.
 * @param[in] err Error code that caused safe-state transition.
 * @note Called from player task context, before on_state(UNCONFIGURED).
 */
typedef void (*player_on_error_cb_t)(void *user_ctx, player_err_t err);

/**
 * @brief Player task config, copied in player_init().
 * @details stack_size is ignored, always PLAYER_STACK_SIZE (static stack).
 *          priority 0 defaults to 5. core_id 0 = PRO (WiFi), 1 = APP (player),
 *          invalid defaults to 1. on_state / on_error may be NULL, then no
 * callback.
 */
typedef struct {
  player_on_state_cb_t on_state;
  player_on_error_cb_t on_error;
  void *user_ctx;
  uint32_t stack_size;
  uint32_t priority;
  int32_t core_id;
} player_cfg_t;

/**
 * send config
 */
typedef struct {
  dec_codec_t codec;
  uint32_t sample_rate;
  uint8_t bits;
  uint8_t channels;
} player_dec_cfg_t;

/**
 * global handles, in reality i2s and buf out are only in player so could be
 * created in init, but buf_in must be a global handle so src can write into
 */
typedef struct {
  i2s_t i2s;
  buf_t buf_in;
  buf_t buf_out;
  dec_t dec;
} player_hw_t;

/**
 * @brief Init player in caller-provided storage.
 * @details Reserves nothing on heap. Zeroes storage, copies cfg, creates
 *          static command queue and static ACK event group.
 *          Does NOT start task, call player_start() after.
 * @param[in] storage Static storage, must outlive player, e.g. static in
 * main.c.
 * @param[in] cfg Config, copied, may be stack-local after call.
 * @param[out] out Handle, points inside storage.
 * @return PLAYER_OK on success, PLAYER_ERR_INVALID_ARG on NULL,
 * PLAYER_ERR_NO_MEM on static create fail.
 */
player_err_t player_init(player_storage_t *storage, const player_cfg_t *cfg,
                         const player_hw_t *hw, player_t *out);

/**
 * @brief Destroy player resources.
 * @details Only after player_cmd_exit(), when task_handle == NULL.
 *          Deletes queue and event group. Does NOT free storage (caller owns
 * it). After destroy, same storage can be reused with player_init().
 * @param[in] play Handle from player_init().
 * @return PLAYER_OK, PLAYER_ERR_INVALID_ARG on NULL, PLAYER_ERR_FAIL if task
 * still running.
 */
player_err_t player_destroy(player_t play);

/**
 * @brief Start player task, pinned to core.
 * @details Uses xTaskCreateStaticPinnedToCore(), no heap. Task name "player".
 *          Task starts in UNCONFIGURED and blocks on command queue.
 * @param[in] play Handle from player_init().
 * @return PLAYER_OK, PLAYER_ERR_INVALID_ARG, PLAYER_ERR_FAIL if already
 * started, PLAYER_ERR_NO_MEM on create fail.
 */
player_err_t player_start(player_t play);

/**
 * @brief Send EXIT, wait for task cleanup, clear task_handle.
 * @details Works from any state. Task does state_cleanup(), sets ACK_OK,
 *          then vTaskDelete(NULL). After this, call player_destroy().
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK_OK.
 * @return PLAYER_OK, TIMEOUT, INVALID_ARG.
 */
player_err_t player_cmd_exit(player_t play, uint32_t timeout_ms);

/**
 * @brief Send PLAY, wait for ACK.
 * @details Valid only from IDLE. Task goes IDLE -> PLAYING, sets ACK_OK,
 *          calls on_state(PLAYING). If not IDLE, task sets ACK_FAIL.
 *          PLAY does NOT start I2S immediately, PLAYING first does prebuffer
 *          (see state_playing() TODO), then decodes first frame, retunes I2S,
 *          then streams.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL (wrong state), BUSY, TIMEOUT.
 */
player_err_t player_cmd_play(player_t play, uint32_t timeout_ms);

/**
 * @brief Send STOP, wait for ACK.
 * @details Valid from PLAYING / PAUSED. Task clears buffers (TODO: flush buf),
 *          goes to IDLE, keeps dec config, sets ACK_OK.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_stop(player_t play, uint32_t timeout_ms);

/**
 * @brief Send PAUSE, wait for ACK.
 * @details Valid from PLAYING. Task stops i2s_write, keeps buffers/config,
 *          goes to PAUSED, sets ACK_OK.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_pause(player_t play, uint32_t timeout_ms);

/**
 * @brief Send RESUME, wait for ACK.
 * @details Valid from PAUSED. Task goes PAUSED -> PLAYING, sets ACK_OK.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_resume(player_t play, uint32_t timeout_ms);

/**
 * @brief Send CONFIG, wait for ACK.
 * @details Valid from UNCONFIGURED / IDLE. Task goes to CONFIGURING,
 *          does buf/dec/i2s setup (TODO: carry play_config_t with codec,
 *          sample_rate/bits/channels), then IDLE, sets ACK_OK.
 *          For now signature carries only timeout, config values are TODO.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_config(player_t play, const player_dec_cfg_t *dec_cfg,
                               uint32_t timeout_ms);

/**
 * @brief Send RESET, wait for ACK.
 * @details Valid from IDLE. Task clears buf/i2s/dec (TODO), goes to
 *          UNCONFIGURED, sets ACK_OK. Use to change codec / release resources.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK.
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_reset(player_t play, uint32_t timeout_ms);

/**
 * @brief Send EOF, wait for ACK.
 * @details Valid from PLAYING. Task drains remaining buf_out (TODO: >=249
 *          drain logic from lab), then goes to UNCONFIGURED, sets ACK_OK.
 *          FAIL during drain is ignored, EOF is not an error.
 * @param[in] play Handle.
 * @param[in] timeout_ms Max wait for ACK (drain may take time).
 * @return PLAYER_OK, FAIL, BUSY, TIMEOUT.
 */
player_err_t player_cmd_eof(player_t play, uint32_t timeout_ms);

/**
 * @brief Get diagnostics snapshot.
 * @param[in] play Handle.
 * @param[out] out Copy of heap / stack HWM / state.
 * @return PLAYER_OK, PLAYER_ERR_INVALID_ARG on NULL.
 */
player_err_t player_get_diag(player_t play, player_diag_t *out);

/**
 * @brief Error code to string for logs.
 * @param[in] err Error code.
 * @return Static string, never NULL.
 */
const char *player_err_to_str(player_err_t err);

#ifdef __cplusplus
}
#endif

#endif // PLAYER_H