#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct lab_s *lab_t;

typedef enum {
  LAB_OK = 0,
  LAB_ERR_INVALID_ARG = -1,
  LAB_ERR_NO_MEM = -2,
  LAB_ERR_TIMEOUT = -3,
  LAB_ERR_FAIL = -4,
  LAB_ERR_BUSY = -5,
} lab_err_t;

typedef enum {
  LAB_STATE_IDLE = 0,
  LAB_STATE_PAUSED,
  LAB_STATE_WORKING,
  LAB_STATE_ERROR,
} lab_state_t;

// Callback: task woła to jak zmieni się stan
// user_ctx to jest to co dałeś w cfg.user_ctx
typedef void (*lab_on_state_cb_t)(void *user_ctx, lab_state_t state);
typedef void (*lab_on_error_cb_t)(void *user_ctx, lab_err_t err);

typedef struct {
  lab_on_state_cb_t on_state; // może być NULL
  lab_on_error_cb_t on_error; // może być NULL
  void *user_ctx;             // Twoje dane, task ich nie rusza, tylko oddaje
  uint32_t stack_size;        // 0 = domyślne 4096
  uint32_t priority;          // 0 = domyślne 5
} lab_cfg_t;

lab_err_t lab_create(const lab_cfg_t *cfg, lab_t *out);
lab_err_t lab_destroy(lab_t lab);
lab_err_t lab_start(lab_t lab);
lab_err_t lab_do_work(lab_t lab, int value, uint32_t timeout_ms);
lab_err_t lab_stop(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_pause(lab_t lab, uint32_t timeout_ms);
const char *lab_err_to_str(lab_err_t err);