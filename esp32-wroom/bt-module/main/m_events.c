#include "m_events.h"

ESP_EVENT_DEFINE_BASE(M_EVENTS);

void m_events_init(void) { ESP_ERROR_CHECK(esp_event_loop_create_default()); }