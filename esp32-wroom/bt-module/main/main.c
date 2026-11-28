#include "m_audio.h"
#include "m_bt.h"
#include "m_buf.h"
#include "m_events.h"
#include "m_nvs.h"
#include "m_sys.h"

void app_main(void) {

  // 1. NVS init
  m_nvs_init();

  // 2. bufs, queues
  ring_buf_handle_t m_ring_buf = ring_buf_create(RING_BUF_SIZE);
  m_events_init();
  m_sys_init();
  m_audio_init(m_ring_buf);
  // 3. BT init
  m_bt_init(m_ring_buf);
}
