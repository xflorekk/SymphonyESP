#include "lab.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/projdefs.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "lab";

/**
 *@brief List of commands
 */
typedef enum {
  CMD_DO_WORK = 0, /*!< when idle to working - start work*/
  CMD_PAUSE,       /*!< when working to paused - keep work*/
  CMD_STOP,        /*!< when working/paused to idle - clear work, keep config*/
  CMD_RESUME,      /*!< when paused to working*/
  CMD_CONFIG,      /*!< when unconfigured/idle to configuring*/
  CMD_EXIT,        /*!< when any to exit*/
  CMD_RESET,       /*!< when idle to unconfigured - clear work/config*/
  CMD_EOF,         /*!< when working to unconfigured */
} cmd_type_t;

typedef struct {
  cmd_type_t type;
  uint32_t seq;
  int value;
} cmd_t;

typedef enum {
  PEND_NONE = 0,
  PEND_CONFIG,
  PEND_RESET,
  PEND_WORK,
  PEND_RESUME,
  PEND_PAUSE,
  PEND_STOP,
  PEND_EXIT,
  PEND_EOF,
} pend_t;

struct lab_s {
  lab_cfg_t cfg;
  pend_t pending;
  TaskHandle_t task_handle;
  QueueHandle_t cmd_q;
  EventGroupHandle_t ack_group;
  uint32_t seq;
  lab_state_t state;
  StaticTask_t task_tcb;
  StackType_t task_stack[LAB_STACK_SIZE];
  StaticQueue_t queue;
  uint8_t queue_buf[LAB_QUEUE_LEN * sizeof(cmd_t)];
  StaticEventGroup_t event_group;
};

_Static_assert(sizeof(struct lab_s) <= sizeof(lab_storage_t),
               "LAB_STORAGE_SIZE too small");

#define ACK_OK (1U << 0)
#define ACK_FAIL (1U << 1)
#define ACK_BUSY (1U << 2)

static lab_err_t lab_error_handler(lab_t lab, lab_err_t err) {
  if (err == LAB_OK) {
    return LAB_OK;
  }
  // stop work, reset, go to unconfigured
  lab->state = LAB_STATE_UNCONFIGURED;
  lab->pending = PEND_NONE;
  if (lab->cfg.on_error) {
    lab->cfg.on_error(lab->cfg.user_ctx, err);
  }
  if (lab->cfg.on_state) {
    lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_UNCONFIGURED);
  }
  return LAB_OK;
}

static lab_err_t lab_cleanup_handler(lab_t lab) { return LAB_OK; }

static lab_err_t lab_reset_handler(lab_t lab, int *work_cfg, int *work) {
  *work = 0;
  *work_cfg = 0;
  return LAB_OK;
}

/**
 *@brief STATE IDLE LOGIC
 *
 *@note 1. Check pending
 *         - EXIT
 *         - CONFIG - go to configuring
 *         - RESET - reset, go to UNCONFIGURED, ACK_OK
 *         - WORK - goto working, ACK_OK
 *      default: pending = NONE
 */
static lab_err_t lab_state_idle(lab_t lab, int *work, int *work_cfg) {
  if (lab->pending == PEND_EXIT) {
    return LAB_OK;
  }
  if (lab->pending == PEND_CONFIG) {
    lab->state = LAB_STATE_CONFIGURING;
    // on_state cb
    lab->pending = PEND_NONE;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_CONFIGURING);
    }
    return LAB_OK;
  }
  if (lab->pending == PEND_RESET) {
    lab->state = LAB_STATE_UNCONFIGURED;
    lab->pending = PEND_NONE;

    lab_reset_handler(lab, work_cfg, work);

    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_UNCONFIGURED);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  if (lab->pending == PEND_WORK) {
    lab->pending = PEND_NONE;
    lab->state = LAB_STATE_WORKING;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_WORKING);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  lab->pending = PEND_NONE;
  return LAB_OK;
}

/**
 *@brief STATE WORKING LOGIC
 *
 *@note 1. Check pending
 *         - EXIT
 *         - STOP, go to IDLE, ACK_OK
 *         - PAUSE, go to PAUSED, ACK_OK
 *         - EOF (do remaining than goto unconfigured, ACK_OK)
 *      2. Do work
 */
static lab_err_t lab_state_working(lab_t lab, int *work, int *work_cfg) {
  if (lab->pending == PEND_EXIT) {
    return LAB_OK;
  }
  if (lab->pending == PEND_STOP) {
    lab->pending = PEND_NONE;
    // clear buffers(work)
    *work = 0;
    lab->state = LAB_STATE_IDLE;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_IDLE);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  if (lab->pending == PEND_PAUSE) {
    lab->pending = PEND_NONE;
    lab->state = LAB_STATE_PAUSED;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_PAUSED);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  if (lab->pending == PEND_EOF) {
    // set eof flag, do eof action
    // in short dry out buffers, here finish counting at 249
    if (*work >= 249) {
      lab->pending = PEND_NONE;
      ESP_LOGI(TAG, "work done!");

      lab_reset_handler(lab, work_cfg, work);

      lab->state = LAB_STATE_UNCONFIGURED;
      if (lab->cfg.on_state) {
        lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_UNCONFIGURED);
      }
      xEventGroupSetBits(lab->ack_group, ACK_OK);
      return LAB_OK;
    }
  }
  // do some work
  *work += 1;
  if (*work == *work_cfg) {
    return LAB_ERR_FAIL;
  }
  if (*work % 100 == 0) {
    ESP_LOGI(TAG, "working=%d", *work);
  }

  vTaskDelay(pdMS_TO_TICKS(10));
  return LAB_OK;
}

/**
 *@brief STATE CONFIGURING LOGIC
 *
 *@note 1. Check pending
 *         - EXIT
 *      2. Configure, goto: IDLE
 *      default: pending = NONE on exit
 */
static lab_err_t lab_state_configuring(lab_t lab, int *pending_cfg,
                                       int *work_cfg) {
  if (lab->pending == PEND_EXIT) {
    return LAB_OK;
  }

  // configuring, set work options
  *work_cfg = *pending_cfg;

  lab->state = LAB_STATE_IDLE;
  lab->pending = PEND_NONE;
  if (lab->cfg.on_state) {
    lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_IDLE);
  }
  xEventGroupSetBits(lab->ack_group, ACK_OK);
  return LAB_OK;
}

/**
 *@brief STATE UNCONFIGURED LOGIC
 *
 *@note 1. Check pending
 *         - EXIT
 *         - CONFIG
 *      default: pending = NONE on exit
 */
static lab_err_t lab_state_unconfigured(lab_t lab) {
  if (lab->pending == PEND_EXIT) {
    return LAB_OK;
  }
  if (lab->pending == PEND_CONFIG) {
    lab->pending = PEND_NONE;
    lab->state = LAB_STATE_CONFIGURING;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_CONFIGURING);
    }
    return LAB_OK;
  }
  lab->pending = PEND_NONE;
  return LAB_OK;
}

/**
 *@brief STATE PAUSED LOGIC
 *
 *@note 1. Check pending
 *         - EXIT
 *         - STOP - clear work, goto IDLE, ACK_OK
 *         - RESUME - goto WORKING, ACK_OK
 *      default: pending = NONE on exit
 */
static lab_err_t lab_state_paused(lab_t lab, int *work) {
  if (lab->pending == PEND_EXIT) {
    return LAB_OK;
  }
  if (lab->pending == PEND_STOP) {
    lab->pending = PEND_NONE;
    // clear buffers
    *work = 0;
    lab->state = LAB_STATE_IDLE;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_IDLE);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  if (lab->pending == PEND_RESUME) {
    lab->pending = PEND_NONE;
    lab->state = LAB_STATE_WORKING;
    if (lab->cfg.on_state) {
      lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_WORKING);
    }
    xEventGroupSetBits(lab->ack_group, ACK_OK);
    return LAB_OK;
  }
  lab->pending = PEND_NONE;
  return LAB_OK;
}

static void lab_task(void *arg) {
  // init
  lab_t lab = (lab_t)arg;
  lab->state = LAB_STATE_UNCONFIGURED;
  cmd_t cmd;
  lab_err_t err = LAB_OK;

  ESP_LOGI(TAG, "task start");
  int work = 0;
  int work_cfg = 0;
  int pending_cfg = 0;

  while (lab->pending != PEND_EXIT) {

    TickType_t queue_wait =
        (lab->state == LAB_STATE_IDLE || lab->state == LAB_STATE_UNCONFIGURED ||
         lab->state == LAB_STATE_PAUSED)
            ? portMAX_DELAY
            : 0;
    if (xQueueReceive(lab->cmd_q, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {
      case CMD_EXIT:
        lab->pending = PEND_EXIT;
        break;

      case CMD_CONFIG:
        if (lab->state != LAB_STATE_UNCONFIGURED &&
            lab->state != LAB_STATE_IDLE) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_CONFIG;
          pending_cfg = cmd.value;
        }
        break;

      case CMD_DO_WORK:
        if (lab->state != LAB_STATE_IDLE) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_WORK;
        }
        break;

      case CMD_PAUSE:
        if (lab->state != LAB_STATE_WORKING) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_PAUSE;
        }
        break;

      case CMD_RESUME:
        if (lab->state != LAB_STATE_PAUSED) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_RESUME;
        }
        break;

      case CMD_STOP:
        if (lab->state != LAB_STATE_WORKING && lab->state != LAB_STATE_PAUSED) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_STOP;
        }
        break;

      case CMD_EOF:
        if (lab->state != LAB_STATE_WORKING) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_EOF;
        }
        break;

      case CMD_RESET:
        if (lab->state != LAB_STATE_IDLE) {
          xEventGroupSetBits(lab->ack_group, ACK_FAIL);
        } else {
          lab->pending = PEND_RESET;
        }
        break;

      default:
        break;
      }
    }

    switch (lab->state) {
    case LAB_STATE_IDLE:
      err = lab_state_idle(lab, &work, &work_cfg);
      break;

    case LAB_STATE_UNCONFIGURED:
      err = lab_state_unconfigured(lab);
      break;

    case LAB_STATE_PAUSED:
      err = lab_state_paused(lab, &work);
      break;

    case LAB_STATE_WORKING:
      err = lab_state_working(lab, &work, &work_cfg);
      break;

    case LAB_STATE_CONFIGURING:
      err = lab_reset_handler(lab, &work_cfg, &work);
      if (err == LAB_OK) {
        err = lab_state_configuring(lab, &pending_cfg, &work_cfg);
      }
      break;

    default:
      break;
    }

    if (err != LAB_OK) {
      lab_error_handler(lab, err);
    }
  }
  // cleanup
  ESP_LOGI(TAG, "task cleanup");
  lab_cleanup_handler(lab);
  xEventGroupSetBits(lab->ack_group, ACK_OK);
  vTaskDelete(NULL);
}

lab_err_t lab_init(lab_storage_t *storage, const lab_cfg_t *cfg, lab_t *out) {
  if (storage == NULL || cfg == NULL || out == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  lab_t lab = (lab_t)storage; // storage to pudlo, lab_s sie w nim miesci
  memset(lab, 0, sizeof(*lab));

  lab->cfg = *cfg;
  // stack jest statyczny, wiec cfg.stack_size ignorujemy, zawsze jest
  // LAB_STACK_SIZE jak chcesz, to mozesz sprawdzic czy cfg.stack_size != 0 i !=
  // LAB_STACK_SIZE to FAIL
  lab->cfg.stack_size = LAB_STACK_SIZE;
  if (lab->cfg.priority == 0) {
    lab->cfg.priority = 5;
  }
  if (lab->cfg.core_id < 0 || lab->cfg.core_id > 1) {
    lab->cfg.core_id = 1; // domyslnie APP CPU
  }

  lab->cmd_q = xQueueCreateStatic(LAB_QUEUE_LEN, sizeof(cmd_t), lab->queue_buf,
                                  &lab->queue);
  lab->ack_group = xEventGroupCreateStatic(&lab->event_group);
  if (lab->cmd_q == NULL || lab->ack_group == NULL) {
    return LAB_ERR_NO_MEM;
  }
  lab->state = LAB_STATE_UNCONFIGURED;
  *out = lab;
  return LAB_OK;
}

/*
lab_err_t lab_create(const lab_cfg_t *cfg, lab_t *out) {
  if (cfg == NULL || out == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  lab_t lab = (lab_t)calloc(1, sizeof(struct lab_s));
  if (lab == NULL) {
    return LAB_ERR_NO_MEM;
  }

  // dodatkowa walidacja danych w cfg
  lab->cfg = *cfg;
  if (lab->cfg.stack_size == 0) {
    lab->cfg.stack_size = 4096;
  }
  if (lab->cfg.priority == 0) {
    lab->cfg.priority = 5;
  }
  lab->cmd_q = xQueueCreate(4, sizeof(cmd_t));
  lab->ack_group = xEventGroupCreate();
  if (lab->cmd_q == NULL || lab->ack_group == NULL) {
    free(lab);
    return LAB_ERR_NO_MEM;
  }
  lab->state = LAB_STATE_UNCONFIGURED;
  *out = lab;
  return LAB_OK;
}
*/

lab_err_t lab_destroy(lab_t lab) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  if (lab->task_handle != NULL) {
    return LAB_ERR_FAIL;
  }
  vQueueDelete(lab->cmd_q);
  vEventGroupDelete(lab->ack_group);
  // free(lab);

  return LAB_OK;
}

lab_err_t lab_start(lab_t lab) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  if (lab->task_handle != NULL) {
    return LAB_ERR_FAIL;
  }
  lab->task_handle = xTaskCreateStaticPinnedToCore(
      lab_task, "lab", LAB_STACK_SIZE, lab, lab->cfg.priority, lab->task_stack,
      &lab->task_tcb, lab->cfg.core_id);
  if (lab->task_handle == NULL) {
    return LAB_ERR_NO_MEM;
  }
  return LAB_OK;
}

lab_err_t lab_exit(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_EXIT, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));
  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));

  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  lab->task_handle = NULL;
  return LAB_OK;
}

lab_err_t lab_stop(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_STOP, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));
  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));

  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

lab_err_t lab_pause(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_PAUSE, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

lab_err_t lab_do_work(lab_t lab, int value, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  if (value <= 0) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);

  cmd_t cmd = {.type = CMD_DO_WORK, .seq = ++lab->seq, .value = value};
  if (xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    return LAB_ERR_FAIL;
  }
  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY, pdTRUE,
                          pdFALSE, pdMS_TO_TICKS(timeout_ms));

  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  if (bits & ACK_BUSY) {
    return LAB_ERR_BUSY;
  }
  return LAB_OK;
}

lab_err_t lab_resume(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_RESUME, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

lab_err_t lab_config(lab_t lab, int value, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_CONFIG, .value = value, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

lab_err_t lab_reset(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_RESET, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

lab_err_t lab_eof(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL | ACK_BUSY);
  cmd_t cmd = {.type = CMD_EOF, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_FAIL | ACK_OK, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0) {
    return LAB_ERR_TIMEOUT;
  }
  if (bits & ACK_FAIL) {
    return LAB_ERR_FAIL;
  }
  return LAB_OK;
}

const char *lab_err_to_str(lab_err_t err) {
  switch (err) {
  case LAB_OK:
    return "OK";
  case LAB_ERR_INVALID_ARG:
    return "INVALID_ARG";
  case LAB_ERR_NO_MEM:
    return "NO_MEM";
  case LAB_ERR_TIMEOUT:
    return "TIMEOUT";
  case LAB_ERR_BUSY:
    return "BUSY";
  case LAB_ERR_NEED_STOP:
    return "NEED_STOP";
  default:
    return "FAIL";
  }
}

lab_err_t lab_diag(lab_t lab, lab_diag_t *out) {
  if (lab == NULL || out == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  out->heap_free = xPortGetFreeHeapSize();
  out->heap_min = xPortGetMinimumEverFreeHeapSize();
  out->state = lab->state;
  if (lab->task_handle != NULL) {
    out->stack_hwm = uxTaskGetStackHighWaterMark(lab->task_handle);
  } else {
    out->stack_hwm = 0;
  }
  return LAB_OK;
}