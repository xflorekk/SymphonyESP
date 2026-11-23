#ifndef LAB_H
#define LAB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>

#define LAB_STACK_SIZE 4096
#define LAB_QUEUE_LEN 4
#define LAB_STORAGE_SIZE 8192

typedef struct {
  uint32_t _opaque[LAB_STORAGE_SIZE / sizeof(uint32_t)];
} lab_storage_t;

typedef struct lab_s *lab_t;

typedef enum {
  LAB_OK = 0,
  LAB_ERR_INVALID_ARG = -1,
  LAB_ERR_NO_MEM = -2,
  LAB_ERR_TIMEOUT = -3,
  LAB_ERR_FAIL = -4,
  LAB_ERR_BUSY = -5,
  LAB_ERR_NEED_STOP = -6,
} lab_err_t;

typedef enum {
  LAB_STATE_IDLE = 0,
  LAB_STATE_WORKING,
  LAB_STATE_CONFIGURING,
  LAB_STATE_UNCONFIGURED,
  LAB_STATE_PAUSED,
} lab_state_t;

typedef struct {
  size_t heap_free;
  size_t heap_min;
  uint32_t stack_hwm;
  lab_state_t state;
} lab_diag_t;

typedef void (*lab_on_state_cb_t)(void *user_ctx, lab_state_t state);
typedef void (*lab_on_error_cb_t)(void *user_ctx, lab_err_t err);

typedef struct {
  lab_on_error_cb_t on_error;
  lab_on_state_cb_t on_state;
  void *user_ctx;
  uint32_t stack_size;
  uint32_t priority;
  int32_t core_id;
} lab_cfg_t;

lab_err_t lab_init(lab_storage_t *storage, const lab_cfg_t *cfg, lab_t *out);
// lab_err_t lab_create(const lab_cfg_t *cfg, lab_t *out);
lab_err_t lab_destroy(lab_t lab);
lab_err_t lab_start(lab_t lab);
lab_err_t lab_exit(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_do_work(lab_t lab, int value, uint32_t timeout_ms);
lab_err_t lab_stop(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_resume(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_pause(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_config(lab_t lab, int value, uint32_t timeout_ms);
lab_err_t lab_eof(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_reset(lab_t lab, uint32_t timeout_ms);
lab_err_t lab_diag(lab_t lab, lab_diag_t *out);

const char *lab_err_to_str(lab_err_t err);

#ifdef __cplusplus
}
#endif

#endif // LAB_H
