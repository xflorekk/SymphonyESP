#include "lab.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "main";

typedef struct {
  const char *name;
  int count;
  int error_count;
  lab_state_t last_state;
} my_ctx_t;

static const char *state_to_str(lab_state_t s) {
  switch (s) {
  case LAB_STATE_UNCONFIGURED:
    return "UNCONFIGURED";
  case LAB_STATE_IDLE:
    return "IDLE";
  case LAB_STATE_CONFIGURING:
    return "CONFIGURING";
  case LAB_STATE_WORKING:
    return "WORKING";
  case LAB_STATE_PAUSED:
    return "PAUSED";
  default:
    return "?";
  }
}

static void on_state(void *user_ctx, lab_state_t state) {
  my_ctx_t *ctx = (my_ctx_t *)user_ctx;
  ctx->count++;
  ctx->last_state = state;
  ESP_LOGI(TAG, "[%s] state=%s count=%d", ctx->name, state_to_str(state),
           ctx->count);
}

static void on_error(void *user_ctx, lab_err_t err) {
  my_ctx_t *ctx = (my_ctx_t *)user_ctx;
  ctx->error_count++;
  ESP_LOGE(TAG, "[%s] error=%s error_count=%d", ctx->name, lab_err_to_str(err),
           ctx->error_count);
}

static void log_diag(const char *when, lab_t lab) {
  lab_diag_t d;
  lab_diag(lab, &d);
  ESP_LOGI(TAG, "%s: state=%s heap_free=%d heap_min=%d stack_hwm=%d slow", when,
           state_to_str(d.state), d.heap_free, d.heap_min, d.stack_hwm);
}

void app_main(void) {
  // storage statyczne, nie na heapie, nie w funkcji jako lokalne
  static lab_storage_t storage1;
  static lab_storage_t storage2;

  my_ctx_t ctx1 = {.name = "lab1",
                   .count = 0,
                   .error_count = 0,
                   .last_state = LAB_STATE_UNCONFIGURED};
  my_ctx_t ctx2 = {.name = "lab2",
                   .count = 0,
                   .error_count = 0,
                   .last_state = LAB_STATE_UNCONFIGURED};

  ESP_LOGI(TAG, "heap przed: free=%d min=%d", xPortGetFreeHeapSize(),
           xPortGetMinimumEverFreeHeapSize());

  // 1. init 2 instancji na statycznym storage, heap nie ma prawa sie ruszyc
  lab_t lab1 = NULL, lab2 = NULL;
  lab_cfg_t cfg1 = {.on_state = on_state,
                    .on_error = on_error,
                    .user_ctx = &ctx1,
                    .priority = 5,
                    .core_id = 1};
  lab_cfg_t cfg2 = {.on_state = on_state,
                    .on_error = on_error,
                    .user_ctx = &ctx2,
                    .priority = 5,
                    .core_id = 1};

  lab_err_t e = lab_init(&storage1, &cfg1, &lab1);
  ESP_LOGI(TAG, "1. init lab1 = %s", lab_err_to_str(e));
  e = lab_init(&storage2, &cfg2, &lab2);
  ESP_LOGI(TAG, "2. init lab2 = %s", lab_err_to_str(e));

  ESP_LOGI(TAG, "heap po init: free=%d min=%d (ma byc takie samo jak przed)",
           xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

  // 2. start, tez statyczny task, heap dalej ma sie nie ruszyc
  e = lab_start(lab1);
  ESP_LOGI(TAG, "3. start lab1 = %s", lab_err_to_str(e));
  e = lab_start(lab2);
  ESP_LOGI(TAG, "4. start lab2 = %s", lab_err_to_str(e));

  ESP_LOGI(TAG, "heap po start: free=%d min=%d (ma byc takie samo jak przed)",
           xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

  vTaskDelay(pdMS_TO_TICKS(100));

  // 3. config obu, work obu, sprawdz czy pending nie wycieka miedzy instancjami
  e = lab_config(lab1, 500, 2000);
  ESP_LOGI(TAG, "5. config lab1 = %s", lab_err_to_str(e));
  e = lab_config(lab2, 500, 2000);
  ESP_LOGI(TAG, "6. config lab2 = %s", lab_err_to_str(e));

  e = lab_do_work(lab1, 10, 2000);
  ESP_LOGI(TAG, "7. do_work lab1 = %s", lab_err_to_str(e));
  e = lab_do_work(lab2, 10, 2000);
  ESP_LOGI(TAG, "8. do_work lab2 = %s", lab_err_to_str(e));

  vTaskDelay(pdMS_TO_TICKS(500));

  // 4. pauza tylko lab1, lab2 ma dalej pracowac
  e = lab_pause(lab1, 2000);
  ESP_LOGI(TAG, "9. pause lab1 = %s (ma byc OK)", lab_err_to_str(e));
  vTaskDelay(pdMS_TO_TICKS(200));

  log_diag("diag lab1", lab1); // ma byc PAUSED
  log_diag("diag lab2", lab2); // ma byc WORKING

  ESP_LOGI(TAG, "10. ctx1 last=%s ctx2 last=%s (ma byc PAUSED / WORKING)",
           state_to_str(ctx1.last_state), state_to_str(ctx2.last_state));

  // 5. resume lab1, stop obu
  e = lab_resume(lab1, 2000);
  ESP_LOGI(TAG, "11. resume lab1 = %s", lab_err_to_str(e));
  e = lab_stop(lab1, 2000);
  ESP_LOGI(TAG, "12. stop lab1 = %s", lab_err_to_str(e));
  e = lab_stop(lab2, 2000);
  ESP_LOGI(TAG, "13. stop lab2 = %s", lab_err_to_str(e));

  log_diag("diag lab1 po stop", lab1);
  log_diag("diag lab2 po stop", lab2);

  // 6. exit + destroy, heap ma wrocic do wartosci z poczatku
  e = lab_exit(lab1, 2000);
  ESP_LOGI(TAG, "14. exit lab1 = %s", lab_err_to_str(e));
  e = lab_exit(lab2, 2000);
  ESP_LOGI(TAG, "15. exit lab2 = %s", lab_err_to_str(e));

  e = lab_destroy(lab1);
  ESP_LOGI(TAG, "16. destroy lab1 = %s", lab_err_to_str(e));
  e = lab_destroy(lab2);
  ESP_LOGI(TAG, "17. destroy lab2 = %s", lab_err_to_str(e));

  ESP_LOGI(TAG, "heap po destroy: free=%d min=%d (ma byc takie samo jak przed)",
           xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

  // 7. re-init na tym samym storage, zeby pokazac ze storage mozna uzyc od nowa
  e = lab_init(&storage1, &cfg1, &lab1);
  ESP_LOGI(TAG, "18. re-init lab1 na tym samym storage = %s (ma byc OK)",
           lab_err_to_str(e));
  e = lab_start(lab1);
  ESP_LOGI(TAG, "19. re-start lab1 = %s", lab_err_to_str(e));
  e = lab_config(lab1, 500, 2000);
  ESP_LOGI(TAG, "20. config po re-init = %s", lab_err_to_str(e));
  e = lab_exit(lab1, 2000);
  ESP_LOGI(TAG, "21. exit po re-init = %s", lab_err_to_str(e));
  e = lab_destroy(lab1);
  ESP_LOGI(TAG, "22. destroy po re-init = %s", lab_err_to_str(e));

  ESP_LOGI(TAG, "done, lab1 count=%d err=%d, lab2 count=%d err=%d", ctx1.count,
           ctx1.error_count, ctx2.count, ctx2.error_count);
}