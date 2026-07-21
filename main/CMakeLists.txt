idf_component_register(
    SRCS
        "app/mcp_sensors.c"
        "wifi/wifi_manager.c"
        "wifi/captive_portal.c"
        "web/web_server.c"
        "web/web_fs.c"
        "sensor/sensor_data.c"
        "sensor/stcc4_sensor.c"
        "sensor/c4004_sensor.c"
        "mcp/mcp_server.c"
        "mcp/mcp_radar_events.c"
    INCLUDE_DIRS
        "app"
        "wifi"
        "web"
        "sensor"
        "mcp"
    REQUIRES
        nvs_flash esp_wifi esp_wifi_remote esp_hosted esp_event esp_netif
        esp_http_server spiffs json mdns driver vrxiaojie__stcc4
        espressif__mcp-c-sdk
)
