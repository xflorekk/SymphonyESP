#ifndef M_BT_H
#define M_BT_H

#include "m_buf.h"

void m_bt_set_local_volume(uint8_t vol_percent);
void m_bt_set_device_name(char *name);
void m_bt_init(ring_buf_handle_t buf);

#endif

// mechanizm "one-shot" (odnawianie rejestracji po każdym zdarzeniu)