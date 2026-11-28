#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lab.h"

static const char *TAG = "main";

// Twoje dane, które chcesz dostać z powrotem w callbacku
typedef struct {
  const char *name;
  int my_value;
  double new_volume;
  int count;
} my_ctx_t;

static void on_state(void *user_ctx, lab_state_t state) {
  // rzutowanie: void* -> my_ctx_t*
  // task nie wie co to jest, my wiemy, bo my to stworzyliśmy
  my_ctx_t *ctx = (my_ctx_t *)user_ctx;
  ctx->count++;
  ESP_LOGI(TAG, "[%s] state=%d, count=%d", ctx->name, (int)state, ctx->count);
}

static void on_error(void *user_ctx, lab_err_t err) {
  my_ctx_t *ctx = (my_ctx_t *)user_ctx;
  ESP_LOGE(TAG, "[%s] error=%s", ctx->name, lab_err_to_str(err));
}

void app_main(void) {
  my_ctx_t my_ctx = {.name = "player1", .count = 0};

  lab_cfg_t cfg = {
      .on_state = on_state,
      .on_error = on_error,
      .user_ctx = &my_ctx, // oddajemy adres naszego structa
      .stack_size = 4096,
      .priority = 5,
  };

  lab_t lab = NULL;
  lab_err_t e = lab_create(&cfg, &lab);
  if (e != LAB_OK) {
    ESP_LOGE(TAG, "create failed: %s", lab_err_to_str(e));
    return;
  }

  lab_start(lab);

  // dobra komenda
  e = lab_do_work(lab, 10, 2000);
  ESP_LOGI(TAG, "do_work(10) = %s", lab_err_to_str(e));

  // nowa dodana komenda
  e = lab_pause(lab, 2000);
  ESP_LOGI(TAG, "pause = %s", lab_err_to_str(e));

  e = lab_do_work(lab, 2, 2000);
  ESP_LOGI(TAG, "do_work(10) = %s", lab_err_to_str(e));

  e = lab_do_work(lab, 1, 2000);
  ESP_LOGI(TAG, "do_work(10) = %s", lab_err_to_str(e));

  // zła komenda, ma wywołać on_error
  e = lab_do_work(lab, -1, 2000);
  ESP_LOGI(TAG, "do_work(-1) = %s", lab_err_to_str(e));

  lab_stop(lab, 2000);
  lab_destroy(lab);
  ESP_LOGI(TAG, "done, count=%d", my_ctx.count);
}