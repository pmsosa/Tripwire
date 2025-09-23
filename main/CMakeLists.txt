idf_component_register(SRCS "door_monitor.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt driver esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_timer esp-tls)