#include "m_audio.h"
#include "m_events.h"
//
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define M_I2S_MCLK GPIO_NUM_0
#define M_I2S_BCLK GPIO_NUM_18
#define M_I2S_WS GPIO_NUM_19
#define M_I2S_DOUT GPIO_NUM_21

static const char *TAG = "AUDIO";
static const int16_t m_silence_buf[2048] = {0};

typedef enum {
  AUDIO_IDLE = 0,
  AUDIO_BUFFERING,
  AUDIO_PLAYING,
  AUDIO_RECONFIGURING,
  AUDIO_FLUSHING
} audio_state_t;

typedef enum { AUDIO_TARGET_OFF = 0, AUDIO_TARGET_ON } audio_target_t;

typedef struct {
  audio_state_t state;
  audio_target_t target;
} audio_cfg_t;
static audio_cfg_t m_audio;

typedef struct {
  bool enabled;
  int sample_rate;
} m_i2s_cfg_t;
static m_i2s_cfg_t m_i2s;

typedef enum {
  CMD_PLAY = 0,
  CMD_PAUSE,
  CMD_SET_SAMPLE_RATE,
  CMD_FLUSH
} audio_cmd_type_t;

typedef struct {
  audio_cmd_type_t type;
  int val;
} audio_cmd_t;

typedef struct {
  uint32_t bytes_played;
  uint32_t underrun_count;
  uint32_t last_log_time;
  size_t min_buffer_level;
  size_t max_buffer_level;
} audio_metrics_t;
static audio_metrics_t m_metrics;

static ring_buf_handle_t m_ring_buf;
static i2s_chan_handle_t m_i2s_handle;
static QueueHandle_t m_audio_queue = NULL;

static bool m_audio_send_cmd(audio_cmd_t *cmd) {
  if (m_audio_queue == NULL) {
    return false;
  }
  if (xQueueSend(m_audio_queue, cmd, pdMS_TO_TICKS(100)) == pdTRUE) {
    return true;
  }
  return false;
}

static bool m_i2s_chan_enable(i2s_chan_handle_t handle, bool enabled) {
  ESP_ERROR_CHECK(i2s_channel_enable(handle));
  return true;
}

static bool m_i2s_chan_disable(i2s_chan_handle_t handle, bool enabled) {
  ESP_ERROR_CHECK(i2s_channel_disable(handle));
  return false;
}

static void m_i2s_clk_cfg(i2s_chan_handle_t handle, int sample_rate) {
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
  clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  i2s_channel_reconfig_std_clock(handle, &clk_cfg);

  ESP_LOGI(TAG, "zmieniono sample rate na: %d", sample_rate);
}

static void m_audio_flush_ringbuf(ring_buf_handle_t buf) {
  size_t item_size;
  void *data;
  while ((data = xRingbufferReceive(buf, &item_size, 0)) != NULL) {
    vRingbufferReturnItem(buf, data);
  }
  ESP_LOGI(TAG, "Bufor audio wyczyszczony.");
}

static void m_audio_event_handler(void *handler_args, esp_event_base_t base,
                                  int32_t id, void *event_data) {
  if (base != M_EVENTS)
    return;

  audio_cmd_t cmd;

  switch (id) {

  case EVENT_AUDIO_PLAY:
    cmd.type = CMD_PLAY;
    if (m_audio_send_cmd(&cmd)) {
      ESP_LOGI(TAG, "event audio play");
    }
    break;

  case EVENT_AUDIO_PAUSE:
    cmd.type = CMD_PAUSE;
    if (m_audio_send_cmd(&cmd)) {
      ESP_LOGI(TAG, "event audio pause");
    }
    break;

  case EVENT_AUDIO_FLUSH:
    cmd.type = CMD_FLUSH;
    m_audio_send_cmd(&cmd);
    ESP_LOGI(TAG, "event audio flush");
    break;

  case EVENT_SAMPLE_RATE:
    if (event_data) {
      cmd.type = CMD_SET_SAMPLE_RATE;
      cmd.val = *((int *)event_data);
      if (m_audio_send_cmd(&cmd)) {
        ESP_LOGI(TAG, "event sample rate");
      }
    }
    break;
  }
}

static void m_audio_task(void *pvParameters) {
  audio_cmd_t cmd;
  size_t written;
  size_t item_size;

  while (1) {

    TickType_t queue_wait = (m_audio.state == AUDIO_IDLE) ? portMAX_DELAY : 0;

    if (xQueueReceive(m_audio_queue, &cmd, queue_wait) == pdTRUE) {
      switch (cmd.type) {

      case CMD_PLAY:
        m_audio.target = AUDIO_TARGET_ON;
        ESP_LOGI(TAG, "on");
        break;

      case CMD_PAUSE:
        m_audio.target = AUDIO_TARGET_OFF;
        ESP_LOGI(TAG, "off");
        break;

      case CMD_SET_SAMPLE_RATE:
        if (m_i2s.sample_rate != cmd.val) {
          m_i2s.sample_rate = cmd.val;
          m_audio.state = AUDIO_RECONFIGURING;
          ESP_LOGI(TAG, "reconfiguring");
        }
        break;

      case CMD_FLUSH:
        m_audio.state = AUDIO_FLUSHING;
        ESP_LOGI(TAG, "flushing");
        break;
      }
    }

    switch (m_audio.state) {
    case AUDIO_IDLE:
      if (m_audio.target == AUDIO_TARGET_ON) {
        if (!m_i2s.enabled) {
          m_i2s.enabled = m_i2s_chan_enable(m_i2s_handle, m_i2s.enabled);
        }
        m_audio.state = AUDIO_BUFFERING;
        ESP_LOGI(TAG, "buffering");
      }
      break;

    case AUDIO_BUFFERING:
      i2s_channel_write(m_i2s_handle, m_silence_buf, sizeof(m_silence_buf),
                        &written, portMAX_DELAY);
      if (m_audio.target == AUDIO_TARGET_OFF) {
        if (m_i2s.enabled) {
          m_i2s.enabled = m_i2s_chan_disable(m_i2s_handle, m_i2s.enabled);
        }
        m_audio.state = AUDIO_IDLE;
        ESP_LOGI(TAG, "idle");
        break;
      }
      if (ring_buf_over_thr(m_ring_buf, RING_BUF_SIZE, RING_BUF_THRESHOLD)) {
        m_audio.state = AUDIO_PLAYING;
        ESP_LOGI(TAG, "playing");
      }
      break;

    case AUDIO_PLAYING: {
      if (m_audio.target == AUDIO_TARGET_OFF) {
        if (m_i2s.enabled) {
          m_i2s.enabled = m_i2s_chan_disable(m_i2s_handle, m_i2s.enabled);
        }
        m_audio.state = AUDIO_IDLE;
        ESP_LOGI(TAG, "idle");
        break;
      }

      size_t current_fill = ring_buf_get_fill(m_ring_buf, RING_BUF_SIZE);
      if (current_fill < m_metrics.min_buffer_level)
        m_metrics.min_buffer_level = current_fill;
      if (current_fill > m_metrics.max_buffer_level)
        m_metrics.max_buffer_level = current_fill;

      uint8_t *data =
          (uint8_t *)ring_buf_read_up_to(m_ring_buf, &item_size, 2048);

      if (data != NULL) {
        i2s_channel_write(m_i2s_handle, data, item_size, &written,
                          portMAX_DELAY);
        ring_buf_return_item(m_ring_buf, (void *)data);

        m_metrics.bytes_played += written;
      } else {
        m_metrics.underrun_count++;
      }

      uint64_t current_time = esp_timer_get_time();
      if ((current_time - m_metrics.last_log_time) >= 10000000) {

        // Wyliczamy bit rate (w kilobajtach na sekundę)
        uint32_t kbps = (m_metrics.bytes_played / 1024) / 10;

        ESP_LOGI(
            TAG,
            "Audio Stats | Fill: %d-%d KB | Rate: %lu KB/s | Underruns: %lu",
            m_metrics.min_buffer_level / 1024,
            m_metrics.max_buffer_level / 1024, kbps, m_metrics.underrun_count);

        // Resetowanie metryk na kolejne 10 sekund
        m_metrics.bytes_played = 0;
        m_metrics.underrun_count = 0;
        m_metrics.min_buffer_level = RING_BUF_SIZE;
        m_metrics.max_buffer_level = 0;
        m_metrics.last_log_time = current_time;
      }
      break;
    }
    case AUDIO_RECONFIGURING:
      if (m_i2s.enabled) {
        m_i2s.enabled = m_i2s_chan_disable(m_i2s_handle, m_i2s.enabled);
      }
      m_i2s_clk_cfg(m_i2s_handle, m_i2s.sample_rate);

      if (m_audio.target == AUDIO_TARGET_ON) {
        if (!m_i2s.enabled) {
          m_i2s.enabled = m_i2s_chan_enable(m_i2s_handle, m_i2s.enabled);
        }
        m_audio.state = AUDIO_BUFFERING;
        ESP_LOGI(TAG, "buffering");
      } else {
        m_audio.state = AUDIO_IDLE;
        ESP_LOGI(TAG, "idle");
      }
      break;

    case AUDIO_FLUSHING:
      if (m_i2s.enabled) {
        m_i2s.enabled = m_i2s_chan_disable(m_i2s_handle, m_i2s.enabled);
      }
      m_audio_flush_ringbuf(m_ring_buf);
      // reset metryk żeby Fill nie pokazywał starego max
      m_metrics.min_buffer_level = RING_BUF_SIZE;
      m_metrics.max_buffer_level = 0;
      m_metrics.bytes_played = 0;

      // po flushu idź do buforowania jeśli mamy grać
      if (m_audio.target == AUDIO_TARGET_ON) {
        if (!m_i2s.enabled) {
          m_i2s.enabled = m_i2s_chan_enable(m_i2s_handle, m_i2s.enabled);
        }
        m_audio.state = AUDIO_BUFFERING;
        ESP_LOGI(TAG, "buffering after flush");
      } else {
        m_audio.state = AUDIO_IDLE;
        ESP_LOGI(TAG, "idle after flush");
      }
      break;
    }
  }
}

void m_audio_init(ring_buf_handle_t buf) {
  m_ring_buf = buf;
  m_audio_queue = xQueueCreate(10, sizeof(audio_cmd_t));

  m_audio = (audio_cfg_t){
      .state = AUDIO_IDLE,
      .target = AUDIO_TARGET_OFF,
  };
  m_i2s = (m_i2s_cfg_t){
      .sample_rate = 16000,
      .enabled = false,
  };
  m_metrics = (audio_metrics_t){
      .bytes_played = 0,
      .underrun_count = 0,
      .last_log_time = 0,
      .min_buffer_level = RING_BUF_SIZE,
      .max_buffer_level = 0,
  };

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;
  chan_cfg.auto_clear = true;

  i2s_new_channel(&chan_cfg, &m_i2s_handle, NULL);

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(m_i2s.sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = M_I2S_MCLK,
              .bclk = M_I2S_BCLK,
              .ws = M_I2S_WS, // LRCK
              .dout = M_I2S_DOUT,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };

  std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  i2s_channel_init_std_mode(m_i2s_handle, &std_cfg);
  // i2s_channel_enable(m_i2s_handle);

  ESP_ERROR_CHECK(esp_event_handler_register(M_EVENTS, ESP_EVENT_ANY_ID,
                                             m_audio_event_handler, NULL));
  xTaskCreatePinnedToCore(m_audio_task, "m_audio", 8192, NULL, 5, NULL, 1);
}