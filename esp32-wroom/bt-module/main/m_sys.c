#include "m_sys.h"
#include "m_events.h"
//
#include <esp_log.h>
#include <string.h>

static const char *TAG = "SYS";

typedef enum { SYS_BT_DISCONNECTED = 0, SYS_BT_CONNECTED } sys_bt_state_t;

typedef struct {
  uint8_t volume_percent;
  sys_bt_state_t bt_state;
} sys_cfg_t;

static sys_cfg_t m_sys;

static void m_sys_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t id, void *event_data) {
  if (base != M_EVENTS)
    return;

  switch (id) {
  case EVENT_BT_CONNECTED:
    m_sys.bt_state = SYS_BT_CONNECTED;
    ESP_LOGI(TAG, "Bluetooth connected.");
    break;

  case EVENT_BT_DISCONNECTED:
    m_sys.bt_state = SYS_BT_DISCONNECTED;
    ESP_LOGI(TAG, "Bluetooth disconnected.");
    break;

  case EVENT_VOLUME_CHANGE: {
    if (event_data) {
      m_sys.volume_percent = *((uint8_t *)event_data);
      ESP_LOGI(TAG, "Volume change request (AVRCP): %d", m_sys.volume_percent);
    }
    break;
  }

  default:
    break;
  }
}

void m_sys_init(void) {
  m_sys = (sys_cfg_t){.volume_percent = 50, .bt_state = SYS_BT_DISCONNECTED};

  ESP_ERROR_CHECK(esp_event_handler_register(M_EVENTS, ESP_EVENT_ANY_ID,
                                             m_sys_event_handler, NULL));
}
