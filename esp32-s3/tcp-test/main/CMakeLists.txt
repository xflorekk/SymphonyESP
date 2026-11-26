idf_component_register(SRCS "main.c" "wifi.c" "srv.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_event esp_netif nvs_flash)