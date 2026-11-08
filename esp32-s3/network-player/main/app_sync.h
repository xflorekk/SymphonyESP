#ifndef APP_SYNC_H
#define APP_SYNC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define EVT_NET_RDO_STOPPED (1 << 0)
#define EVT_NET_RDO_STARTED (1 << 1)
#define EVT_NET_RDO_ERROR (1 << 2)
#define EVT_NET_RDO_READY (1 << 3)
#define EVT_NET_RDO_PLAYING (1 << 4)
#define EVT_NET_RDO_SHUTDOWN (1 << 5)

#define EVT_NET_SRV_STOPPED (1 << 6)
#define EVT_NET_SRV_STARTED (1 << 7)
#define EVT_NET_SRV_ERROR (1 << 8)
#define EVT_NET_SRV_READY (1 << 9)
#define EVT_NET_SRV_PLAYING (1 << 10)
#define EVT_NET_SRV_SHUTDOWN (1 << 11)

#define EVT_BT_RCV_STOPPED (1 << 12)
#define EVT_BT_RCV_STARTED (1 << 13)
#define EVT_BT_RCV_ERROR (1 << 14)
#define EVT_BT_RCV_READY (1 << 15)
#define EVT_BT_RCV_PLAYING (1 << 16)
#define EVT_BT_RCV_SHUTDOWN (1 << 17)

#define EVT_PLAYER_STOPPED (1 << 18)
#define EVT_PLAYER_READY (1 << 19)
#define EVT_PLAYER_ERROR (1 << 20)
#define EVT_PLAYER_PLAYING (1 << 21)

extern EventGroupHandle_t g_app_sync_evt;

#endif