idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash bt lwip esp_timer)