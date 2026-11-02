#include "app.h"
#include "nvs.h"
#include "tcp_cli.h"
#include "tcp_srv.h"
#include "wifi.h"

void app_main(void) {
  /*
        1. NVS Flash init, WiFi init
        2. Buffers, queues init
        3. WiFi start
    */

  nvs_init();
  wifi_init();

  tcp_srv_init();
  app_init();

  wifi_start();
}
