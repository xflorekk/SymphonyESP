#include "lab.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "lab";

typedef enum {
  CMD_DO_WORK = 0,
  CMD_PAUSE,
  CMD_STOP,
} cmd_type_t;

typedef struct {
  cmd_type_t type;
  uint32_t seq;
  int value; // tylko dla DO_WORK
} cmd_t;

// Prawdziwy struct, tylko w .c
struct lab_s {
  lab_cfg_t cfg;
  TaskHandle_t task_handle;
  QueueHandle_t cmd_q;
  EventGroupHandle_t ack_group;
  uint32_t seq;
  lab_state_t state;
  bool should_exit;
};

#define ACK_OK (1U << 0)
#define ACK_FAIL (1U << 1)

static void lab_task(void *arg) {
  lab_t lab = (lab_t)arg;
  cmd_t cmd;
  ESP_LOGI(TAG, "task start");

  while (!lab->should_exit) {
    // czekaj na komendę, jak nie ma komendy to śpij
    if (xQueueReceive(lab->cmd_q, &cmd, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (cmd.type == CMD_DO_WORK) {
      if (lab->state == LAB_STATE_WORKING) {
        if (lab->cfg.on_error) {
          lab->cfg.on_error(lab->cfg.user_ctx, LAB_ERR_BUSY);
          continue;
        }
      }
      lab->state = LAB_STATE_WORKING;
      // wołamy callback, oddajemy user_ctx z powrotem
      if (lab->cfg.on_state) {
        lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_WORKING);
      }

      // udawana praca: jak value < 0 to błąd
      if (cmd.value < 0) {
        lab->state = LAB_STATE_ERROR;
        if (lab->cfg.on_error) {
          lab->cfg.on_error(lab->cfg.user_ctx, LAB_ERR_INVALID_ARG);
        }
        xEventGroupSetBits(lab->ack_group, ACK_FAIL);
      } else {
        ESP_LOGI(TAG, "doing work, value=%d", cmd.value);
        vTaskDelay(pdMS_TO_TICKS(500));
        lab->state = LAB_STATE_IDLE;
        if (lab->cfg.on_state) {
          lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_IDLE);
        }
        xEventGroupSetBits(lab->ack_group, ACK_OK);
      }
    } else if (cmd.type == CMD_STOP) {
      lab->state = LAB_STATE_IDLE;
      xEventGroupSetBits(lab->ack_group, ACK_OK);
      lab->should_exit = true;
    } else if (cmd.type == CMD_PAUSE) {
      lab->state = LAB_STATE_PAUSED;
      if (lab->cfg.on_state) {
        lab->cfg.on_state(lab->cfg.user_ctx, LAB_STATE_PAUSED);
      }
      xEventGroupSetBits(lab->ack_group, ACK_OK);
    }
  }
  ESP_LOGI(TAG, "task exit");
  vTaskDelete(NULL);
}

lab_err_t lab_create(const lab_cfg_t *cfg, lab_t *out) {
  if (cfg == NULL || out == NULL)
    return LAB_ERR_INVALID_ARG;
  lab_t lab = (lab_t)malloc(sizeof(struct lab_s));
  if (lab == NULL)
    return LAB_ERR_NO_MEM;
  memset(lab, 0, sizeof(struct lab_s));
  lab->cfg = *cfg;
  if (lab->cfg.stack_size == 0)
    lab->cfg.stack_size = 4096;
  if (lab->cfg.priority == 0)
    lab->cfg.priority = 5;
  lab->cmd_q = xQueueCreate(4, sizeof(cmd_t));
  lab->ack_group = xEventGroupCreate();
  if (lab->cmd_q == NULL || lab->ack_group == NULL) {
    free(lab);
    return LAB_ERR_NO_MEM;
  }
  lab->state = LAB_STATE_IDLE;
  *out = lab;
  return LAB_OK;
}

lab_err_t lab_destroy(lab_t lab) {
  if (lab == NULL)
    return LAB_ERR_INVALID_ARG;
  if (lab->task_handle != NULL)
    return LAB_ERR_FAIL; // najpierw lab_stop
  vQueueDelete(lab->cmd_q);
  vEventGroupDelete(lab->ack_group);
  free(lab);
  return LAB_OK;
}

lab_err_t lab_start(lab_t lab) {
  if (lab == NULL)
    return LAB_ERR_INVALID_ARG;
  if (lab->task_handle != NULL)
    return LAB_ERR_FAIL;
  BaseType_t ok = xTaskCreate(lab_task, "lab", lab->cfg.stack_size, lab,
                              lab->cfg.priority, &lab->task_handle);
  if (ok != pdPASS)
    return LAB_ERR_NO_MEM;
  return LAB_OK;
}

// To jest wzorzec który potem jest w play_config / play_play / play_stop:
// 1. wyczyść ACK
// 2. wyślij komendę do kolejki
// 3. czekaj na ACK z timeoutem
// 4. zwróć błąd jak timeout albo FAIL
lab_err_t lab_do_work(lab_t lab, int value, uint32_t timeout_ms) {
  if (lab == NULL)
    return LAB_ERR_INVALID_ARG;
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL);
  cmd_t cmd = {.type = CMD_DO_WORK, .seq = ++lab->seq, .value = value};
  if (xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    return LAB_ERR_FAIL;
  }
  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_OK | ACK_FAIL, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0)
    return LAB_ERR_TIMEOUT;
  if (bits & ACK_FAIL)
    return LAB_ERR_FAIL;
  return LAB_OK;
}

lab_err_t lab_stop(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL)
    return LAB_ERR_INVALID_ARG;
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL);
  cmd_t cmd = {.type = CMD_STOP, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));
  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_OK | ACK_FAIL, pdTRUE, pdFALSE,
                          pdMS_TO_TICKS(timeout_ms));
  if (bits == 0)
    return LAB_ERR_TIMEOUT;
  lab->task_handle = NULL;
  return LAB_OK;
}

lab_err_t lab_pause(lab_t lab, uint32_t timeout_ms) {
  if (lab == NULL) {
    return LAB_ERR_INVALID_ARG;
  }
  xEventGroupClearBits(lab->ack_group, ACK_OK | ACK_FAIL);
  cmd_t cmd = {.type = CMD_PAUSE, .seq = ++lab->seq};
  xQueueSend(lab->cmd_q, &cmd, pdMS_TO_TICKS(100));

  EventBits_t bits =
      xEventGroupWaitBits(lab->ack_group, ACK_OK | ACK_FAIL, pdTRUE, pdFALSE,
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
  default:
    return "FAIL";
  }
}