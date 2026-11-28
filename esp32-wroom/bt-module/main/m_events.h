#ifndef M_EVENTS_H
#define M_EVENTS_H

#include <esp_event.h>
#include <stdint.h>

ESP_EVENT_DECLARE_BASE(M_EVENTS);

typedef enum {
  EVENT_AUDIO_PLAY = 0,
  EVENT_AUDIO_PAUSE,
  EVENT_AUDIO_FLUSH,
  EVENT_SAMPLE_RATE,
  EVENT_VOLUME_CHANGE,
  EVENT_BT_CONNECTED,
  EVENT_BT_DISCONNECTED,
  EVENT_NEW_METADATA
} m_event_id_t;

void m_events_init(void);

#endif